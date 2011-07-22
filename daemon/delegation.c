/* Copyright (c) 2010
 * The Regents of the University of Michigan
 * All Rights Reserved
 *
 * Permission is granted to use, copy and redistribute this software
 * for noncommercial education and research purposes, so long as no
 * fee is charged, and so long as the name of the University of Michigan
 * is not used in any advertising or publicity pertaining to the use
 * or distribution of this software without specific, written prior
 * authorization.  Permission to modify or otherwise create derivative
 * works of this software is not granted.
 *
 * This software is provided as is, without representation or warranty
 * of any kind either express or implied, including without limitation
 * the implied warranties of merchantability, fitness for a particular
 * purpose, or noninfringement.  The Regents of the University of
 * Michigan shall not be liable for any damages, including special,
 * indirect, incidental, or consequential damages, with respect to any
 * claim arising out of or in connection with the use of the software,
 * even if it has been or is hereafter advised of the possibility of
 * such damages.
 */

#include "delegation.h"
#include "nfs41_ops.h"
#include "util.h"
#include "daemon_debug.h"


#define DGLVL 2 /* dprintf level for delegation logging */


/* allocation and reference counting */
static int delegation_create(
    IN const nfs41_path_fh *parent,
    IN const nfs41_path_fh *file,
    IN const open_delegation4 *delegation,
    OUT nfs41_delegation_state **deleg_out)
{
    nfs41_delegation_state *state;
    int status = NO_ERROR;

    state = calloc(1, sizeof(nfs41_delegation_state));
    if (state == NULL) {
        status = GetLastError();
        goto out;
    }

    memcpy(&state->state, delegation, sizeof(open_delegation4));

    abs_path_copy(&state->path, file->path);
    path_fh_init(&state->file, &state->path);
    fh_copy(&state->file.fh, &file->fh);
    path_fh_init(&state->parent, &state->path);
    last_component(state->path.path, state->file.name.name,
        &state->parent.name);
    fh_copy(&state->parent.fh, &parent->fh);

    list_init(&state->client_entry);
    state->status = DELEGATION_GRANTED;
    InitializeSRWLock(&state->lock);
    InitializeConditionVariable(&state->cond);
    state->ref_count = 1;
    *deleg_out = state;
out:
    return status;
}

void nfs41_delegation_ref(
    IN nfs41_delegation_state *state)
{
    const LONG count = InterlockedIncrement(&state->ref_count);
    dprintf(DGLVL, "nfs41_delegation_ref(%s) count %d\n",
        state->path.path, count);
}

void nfs41_delegation_deref(
    IN nfs41_delegation_state *state)
{
    const LONG count = InterlockedDecrement(&state->ref_count);
    dprintf(DGLVL, "nfs41_delegation_deref(%s) count %d\n",
        state->path.path, count);
    if (count == 0)
        free(state);
}


/* delegation return */
#define open_entry(pos) list_container(pos, nfs41_open_state, client_entry)

static int open_deleg_cmp(const struct list_entry *entry, const void *value)
{
    nfs41_open_state *open = open_entry(entry);
    int result = -1;

    /* open must match the delegation and have no open stateid */
    AcquireSRWLockShared(&open->lock);
    if (open->delegation.state != value) goto out;
    if (open->do_close) goto out;
    result = 0;
out:
    ReleaseSRWLockShared(&open->lock);
    return result;
}

/* find the first open that needs recovery */
static nfs41_open_state* deleg_open_find(
    IN struct client_state *state,
    IN const nfs41_delegation_state *deleg)
{
    struct list_entry *entry;
    nfs41_open_state *open = NULL;

    EnterCriticalSection(&state->lock);
    entry = list_search(&state->opens, deleg, open_deleg_cmp);
    if (entry) {
        open = open_entry(entry);
        nfs41_open_state_ref(open); /* return a reference */
    }
    LeaveCriticalSection(&state->lock);
    return open;
}

static void delegation_remove(
    IN nfs41_client *client,
    IN nfs41_delegation_state *deleg)
{
    struct list_entry *entry;

    /* remove from the client's list */
    EnterCriticalSection(&client->state.lock);
    list_remove(&deleg->client_entry);

    /* remove from each associated open */
    list_for_each(entry, &client->state.opens) {
        nfs41_open_state *open = open_entry(entry);
        AcquireSRWLockExclusive(&open->lock);
        if (open->delegation.state == deleg) {
            /* drop the delegation reference */
            nfs41_delegation_deref(open->delegation.state);
            open->delegation.state = NULL;
        }
        ReleaseSRWLockExclusive(&open->lock);
    }
    LeaveCriticalSection(&client->state.lock);

    /* signal threads waiting on delegreturn */
    AcquireSRWLockExclusive(&deleg->lock);
    deleg->status = DELEGATION_RETURNED;
    WakeAllConditionVariable(&deleg->cond);
    ReleaseSRWLockExclusive(&deleg->lock);

    /* release the client's reference */
    nfs41_delegation_deref(deleg);
}

#pragma warning (disable : 4706) /* assignment within conditional expression */

static int delegation_return(
    IN nfs41_client *client,
    IN nfs41_delegation_state *deleg,
    IN bool_t truncate,
    IN bool_t try_recovery)
{
    stateid_arg stateid;
    int status = NFS4_OK;

    /* recover opens associated with the delegation */
    nfs41_open_state *open;
    while (open = deleg_open_find(&client->state, deleg)) {
        status = nfs41_delegation_to_open(open, try_recovery);
        nfs41_open_state_deref(open);

        if (status == NFS4ERR_BADSESSION)
            goto out;
    }

    /* return the delegation */
    stateid.type = STATEID_DELEG_FILE;
    stateid.open = NULL;
    stateid.delegation = deleg;
    AcquireSRWLockShared(&deleg->lock);
    memcpy(&stateid.stateid, &deleg->state.stateid, sizeof(stateid4));
    ReleaseSRWLockShared(&deleg->lock);

    status = nfs41_delegreturn(client->session,
        &deleg->file, &stateid, try_recovery);
    if (status == NFS4ERR_BADSESSION)
        goto out;

    delegation_remove(client, deleg);
out:
    return status;
}


/* open delegation */
int nfs41_delegation_granted(
    IN nfs41_session *session,
    IN nfs41_path_fh *parent,
    IN nfs41_path_fh *file,
    IN open_delegation4 *delegation,
    IN bool_t try_recovery,
    OUT nfs41_delegation_state **deleg_out)
{
    stateid_arg stateid;
    nfs41_client *client = session->client;
    nfs41_delegation_state *state;
    int status = NO_ERROR;

    if (delegation->type != OPEN_DELEGATE_READ &&
        delegation->type != OPEN_DELEGATE_WRITE)
        goto out;

    if (delegation->recalled)
        goto out_return;

    /* allocate the delegation state */
    status = delegation_create(parent, file, delegation, &state);
    if (status)
        goto out_return;

    /* register the delegation with the client */
    EnterCriticalSection(&client->state.lock);
    /* XXX: check for duplicates by fh and stateid? */
    list_add_tail(&client->state.delegations, &state->client_entry);
    LeaveCriticalSection(&client->state.lock);

    nfs41_delegation_ref(state); /* return a reference */
    *deleg_out = state;
out:
    return status;

out_return: /* return the delegation on failure */
    memcpy(&stateid.stateid, &delegation->stateid, sizeof(stateid4));
    stateid.type = STATEID_DELEG_FILE;
    stateid.open = NULL;
    stateid.delegation = NULL;
    nfs41_delegreturn(session, file, &stateid, try_recovery);
    goto out;
}

#define deleg_entry(pos) list_container(pos, nfs41_delegation_state, client_entry)

static int deleg_fh_cmp(const struct list_entry *entry, const void *value)
{
    const nfs41_fh *lhs = &deleg_entry(entry)->file.fh;
    const nfs41_fh *rhs = (const nfs41_fh*)value;
    if (lhs->superblock != rhs->superblock) return -1;
    if (lhs->fileid != rhs->fileid) return -1;
    return 0;
}

static bool_t delegation_compatible(
    IN enum open_delegation_type4 type,
    IN uint32_t create,
    IN uint32_t access,
    IN uint32_t deny)
{
    switch (type) {
    case OPEN_DELEGATE_WRITE:
        /* An OPEN_DELEGATE_WRITE delegation allows the client to handle,
         * on its own, all opens. */
        return TRUE;

    case OPEN_DELEGATE_READ:
        /* An OPEN_DELEGATE_READ delegation allows a client to handle,
         * on its own, requests to open a file for reading that do not
         * deny OPEN4_SHARE_ACCESS_READ access to others. */
        if (create == OPEN4_CREATE)
            return FALSE;
        if (access & OPEN4_SHARE_ACCESS_WRITE || deny & OPEN4_SHARE_DENY_READ)
            return FALSE;
        return TRUE;

    default:
        return FALSE;
    }
}

static int delegation_find(
    IN nfs41_client *client,
    IN const void *value,
    IN list_compare_fn cmp,
    OUT nfs41_delegation_state **deleg_out)
{
    struct list_entry *entry;
    int status = NFS4ERR_BADHANDLE;

    EnterCriticalSection(&client->state.lock);
    entry = list_search(&client->state.delegations, value, cmp);
    if (entry) {
        *deleg_out = deleg_entry(entry);
        nfs41_delegation_ref(*deleg_out);
        status = NFS4_OK;
    }
    LeaveCriticalSection(&client->state.lock);
    return status;
}

static int delegation_truncate(
    IN nfs41_delegation_state *deleg,
    IN nfs41_client *client,
    IN stateid_arg *stateid,
    IN uint32_t mode,
    IN nfs41_file_info *info)
{
    nfs41_superblock *superblock = deleg->file.fh.superblock;

    /* use SETATTR to truncate the file */
    info->attrmask.arr[0] = FATTR4_WORD0_SIZE;
    info->attrmask.arr[1] = FATTR4_WORD1_MODE |
        FATTR4_WORD1_TIME_CREATE | FATTR4_WORD1_TIME_MODIFY_SET;
    info->attrmask.count = 2;

    info->size = 0;
    info->mode = mode;
    get_nfs_time(&info->time_create);
    get_nfs_time(&info->time_modify);
    info->time_delta = &superblock->time_delta;

    /* mask out unsupported attributes */
    nfs41_superblock_supported_attrs(superblock, &info->attrmask);

    return nfs41_setattr(client->session, &deleg->file, stateid, info);
}

int nfs41_delegate_open(
    IN nfs41_client *client,
    IN nfs41_path_fh *file,
    IN uint32_t create,
    IN uint32_t mode,
    IN uint32_t access,
    IN uint32_t deny,
    OUT nfs41_delegation_state **deleg_out,
    OUT nfs41_file_info *info)
{
    nfs41_delegation_state *deleg;
    stateid_arg stateid;
    int status;

    /* search for a delegation with this filehandle */
    status = delegation_find(client, &file->fh, deleg_fh_cmp, &deleg);
    if (status)
        goto out;

    AcquireSRWLockExclusive(&deleg->lock);
    if (deleg->status != DELEGATION_GRANTED) {
        /* the delegation is being returned, wait for it to finish */
        while (deleg->status != DELEGATION_RETURNED)
            SleepConditionVariableSRW(&deleg->cond, &deleg->lock, INFINITE, 0);
        status = NFS4ERR_BADHANDLE;
    }
    else if (!delegation_compatible(deleg->state.type, create, access, deny)) {
#ifdef DELEGATION_RETURN_ON_CONFLICT
        /* this open will conflict, start the delegation return */
        deleg->status = DELEGATION_RETURNING;
        status = NFS4ERR_DELEG_REVOKED;
#else
        status = NFS4ERR_BADHANDLE;
#endif
    } else if (create == OPEN4_CREATE) {
        /* copy the stateid for SETATTR */
        stateid.open = NULL;
        stateid.delegation = deleg;
        stateid.type = STATEID_DELEG_FILE;
        memcpy(&stateid.stateid, &deleg->state.stateid, sizeof(stateid4));
    }
    ReleaseSRWLockExclusive(&deleg->lock);

    if (status == NFS4ERR_DELEG_REVOKED)
        goto out_return;
    if (status)
        goto out_deleg;

    if (create == OPEN4_CREATE) {
        /* write delegations allow us to simulate OPEN4_CREATE with SETATTR */
        status = delegation_truncate(deleg, client, &stateid, mode, info);
        if (status)
            goto out_deleg;
    }

    /* TODO: check access against deleg->state.permissions or send ACCESS */

    *deleg_out = deleg;
    status = NFS4_OK;
out:
    return status;

out_return:
    delegation_return(client, deleg, create == OPEN4_CREATE, TRUE);

out_deleg:
    nfs41_delegation_deref(deleg);
    goto out;
}

int nfs41_delegation_to_open(
    IN nfs41_open_state *open,
    IN bool_t try_recovery)
{
    open_delegation4 ignore;
    open_claim4 claim;
    stateid4 open_stateid = { 0 };
    stateid_arg deleg_stateid;
    int status = NFS4_OK;

    AcquireSRWLockExclusive(&open->lock);
    if (open->delegation.state == NULL) /* no delegation to reclaim */
        goto out_unlock;

    if (open->do_close) /* already have an open stateid */
        goto out_unlock;

    /* if another thread is reclaiming the open stateid,
     * wait for it to finish before returning success */
    if (open->delegation.reclaim) {
        do {
            SleepConditionVariableSRW(&open->delegation.cond, &open->lock,
                INFINITE, 0);
        } while (open->delegation.reclaim);
        if (open->do_close)
            goto out_unlock;
    }
    open->delegation.reclaim = 1;

    AcquireSRWLockShared(&open->delegation.state->lock);
    deleg_stateid.open = open;
    deleg_stateid.delegation = NULL;
    deleg_stateid.type = STATEID_DELEG_FILE;
    memcpy(&deleg_stateid.stateid, &open->delegation.state->state.stateid,
        sizeof(stateid4));
    ReleaseSRWLockShared(&open->delegation.state->lock);

    ReleaseSRWLockExclusive(&open->lock);

    /* send OPEN with CLAIM_DELEGATE_CUR */
    claim.claim = CLAIM_DELEGATE_CUR;
    claim.u.deleg_cur.delegate_stateid = &deleg_stateid;
    claim.u.deleg_cur.name = &open->file.name;

    status = nfs41_open(open->session, &open->parent, &open->file,
        &open->owner, &claim, open->share_access, open->share_deny,
        OPEN4_NOCREATE, 0, 0, try_recovery, &open_stateid, &ignore, NULL);

    AcquireSRWLockExclusive(&open->lock);
    if (status == NFS4_OK) {
        /* save the new open stateid */
        memcpy(&open->stateid, &open_stateid, sizeof(stateid4));
        open->do_close = 1;
    } else if (status == NFS4ERR_BAD_STATEID && open->do_close) {
        /* something triggered client state recovery, and the open stateid
         * has already been reclaimed; see recover_stateid_delegation() */
        status = NFS4_OK;
    }
    open->delegation.reclaim = 0;

    /* signal anyone waiting on the open stateid */
    WakeAllConditionVariable(&open->delegation.cond);
out_unlock:
    ReleaseSRWLockExclusive(&open->lock);
    if (status)
        eprintf("nfs41_delegation_to_open(%p) failed with %s\n",
            open, nfs_error_string(status));
    return status;
}


/* synchronous delegation return */
#ifdef DELEGATION_RETURN_ON_CONFLICT
int nfs41_delegation_return(
    IN nfs41_session *session,
    IN nfs41_path_fh *file,
    IN enum open_delegation_type4 access,
    IN bool_t truncate)
{
    nfs41_client *client = session->client;
    nfs41_delegation_state *deleg;
    int status;

    /* find a delegation for this file */
    status = delegation_find(client, &file->fh, deleg_fh_cmp, &deleg);
    if (status)
        goto out;

    AcquireSRWLockExclusive(&deleg->lock);
    if (deleg->status == DELEGATION_GRANTED) {
        /* return unless delegation is write and access is read */
        if (deleg->state.type != OPEN_DELEGATE_WRITE
            || access != OPEN_DELEGATE_READ) {
            deleg->status = DELEGATION_RETURNING;
            status = NFS4ERR_DELEG_REVOKED;
        }
    } else {
        /* the delegation is being returned, wait for it to finish */
        while (deleg->status != DELEGATION_RETURNED)
            SleepConditionVariableSRW(&deleg->cond, &deleg->lock, INFINITE, 0);
        status = NFS4ERR_BADHANDLE;
    }
    ReleaseSRWLockExclusive(&deleg->lock);

    if (status == NFS4ERR_DELEG_REVOKED) {
        delegation_return(client, deleg, truncate, TRUE);
        status = NFS4_OK;
    }

    nfs41_delegation_deref(deleg);
out:
    return status;
}
#endif


/* asynchronous delegation recall */
struct recall_thread_args {
    nfs41_client            *client;
    nfs41_delegation_state  *delegation;
    bool_t                  truncate;
};

static unsigned int WINAPI delegation_recall_thread(void *args)
{
    struct recall_thread_args *recall = (struct recall_thread_args*)args;

    delegation_return(recall->client, recall->delegation, recall->truncate, TRUE);

    /* clean up thread arguments */
    nfs41_delegation_deref(recall->delegation);
    nfs41_root_deref(recall->client->root);
    free(recall);
    return 0;
}

static int deleg_stateid_cmp(const struct list_entry *entry, const void *value)
{
    const stateid4 *lhs = &deleg_entry(entry)->state.stateid;
    const stateid4 *rhs = (const stateid4*)value;
    return memcmp(lhs->other, rhs->other, NFS4_STATEID_OTHER);
}

int nfs41_delegation_recall(
    IN nfs41_client *client,
    IN nfs41_fh *fh,
    IN const stateid4 *stateid,
    IN bool_t truncate)
{
    nfs41_delegation_state *deleg;
    struct recall_thread_args *args;
    int status;

    dprintf(2, "--> nfs41_delegation_recall()\n");

    /* search for the delegation by stateid instead of filehandle;
     * deleg_fh_cmp() relies on a proper superblock and fileid,
     * which we don't get with CB_RECALL */
    status = delegation_find(client, stateid, deleg_stateid_cmp, &deleg);
    if (status)
        goto out;

    AcquireSRWLockExclusive(&deleg->lock);
    if (deleg->state.recalled) {
        /* return BADHANDLE if we've already responded to CB_RECALL */
        status = NFS4ERR_BADHANDLE;
    } else {
        deleg->state.recalled = 1;

        if (deleg->status == DELEGATION_GRANTED) {
            /* start the delegation return */
            deleg->status = DELEGATION_RETURNING;
            status = NFS4ERR_DELEG_REVOKED;
        } /* else return NFS4_OK */
    }
    ReleaseSRWLockExclusive(&deleg->lock);

    if (status != NFS4ERR_DELEG_REVOKED)
        goto out_deleg;

    /* allocate thread arguments */
    args = calloc(1, sizeof(struct recall_thread_args));
    if (args == NULL) {
        status = NFS4ERR_SERVERFAULT;
        eprintf("nfs41_delegation_recall() failed to allocate arguments\n");
        goto out_deleg;
    }

    /* hold a reference on the root */
    nfs41_root_ref(client->root);
    args->client = client;
    args->delegation = deleg;
    args->truncate = truncate;

    /* the callback thread can't make rpc calls, so spawn a separate thread */
    if (_beginthreadex(NULL, 0, delegation_recall_thread, args, 0, NULL) == 0) {
        status = NFS4ERR_SERVERFAULT;
        eprintf("nfs41_delegation_recall() failed to start thread\n");
        goto out_args;
    }
    status = NFS4_OK;
out:
    dprintf(DGLVL, "<-- nfs41_delegation_recall() returning %s\n",
        nfs_error_string(status));
    return status;

out_args:
    free(args);
    nfs41_root_deref(client->root);
out_deleg:
    nfs41_delegation_deref(deleg);
    goto out;
}

void nfs41_client_delegation_free(
    IN nfs41_client *client)
{
    struct list_entry *entry, *tmp;

    EnterCriticalSection(&client->state.lock);
    list_for_each_tmp (entry, tmp, &client->state.delegations) {
        list_remove(entry);
        nfs41_delegation_deref(deleg_entry(entry));
    }
    LeaveCriticalSection(&client->state.lock);
}


static int delegation_recovery_status(
    IN nfs41_delegation_state *deleg)
{
    int status = NFS4_OK;

    AcquireSRWLockExclusive(&deleg->lock);
    if (deleg->status == DELEGATION_GRANTED) {
        if (deleg->revoked) {
            deleg->status = DELEGATION_RETURNED;
            status = NFS4ERR_BADHANDLE;
        } else if (deleg->state.recalled) {
            deleg->status = DELEGATION_RETURNING;
            status = NFS4ERR_DELEG_REVOKED;
        }
    }
    ReleaseSRWLockExclusive(&deleg->lock);
    return status;
}

int nfs41_client_delegation_recovery(
    IN nfs41_client *client)
{
    struct list_entry *entry, *tmp;
    nfs41_delegation_state *deleg;
    int status = NFS4_OK;

    list_for_each_tmp(entry, tmp, &client->state.delegations) {
        deleg = list_container(entry, nfs41_delegation_state, client_entry);

        status = delegation_recovery_status(deleg);
        switch (status) {
        case NFS4ERR_DELEG_REVOKED:
            /* the delegation was reclaimed, but flagged as recalled;
             * return it with try_recovery=FALSE */
            status = delegation_return(client, deleg, FALSE, FALSE);
            break;

        case NFS4ERR_BADHANDLE:
            /* reclaim failed, so we have no delegation state on the server;
             * 'forget' the delegation without trying to return it */
            delegation_remove(client, deleg);
            status = NFS4_OK;
            break;
        }

        if (status == NFS4ERR_BADSESSION)
            goto out;
    }

    /* use DELEGPURGE to indicate that we're done reclaiming delegations */
    status = nfs41_delegpurge(client->session);

    /* support for DELEGPURGE is optional; ignore any errors but BADSESSION */
    if (status != NFS4ERR_BADSESSION)
        status = NFS4_OK;
out:
    return status;
}