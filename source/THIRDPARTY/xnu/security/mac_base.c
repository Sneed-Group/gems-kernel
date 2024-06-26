/*
 * Copyright (c) 2007-2020 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*-
 * Copyright (c) 1999, 2000, 2001, 2002 Robert N. M. Watson
 * Copyright (c) 2001 Ilmar S. Habibulin
 * Copyright (c) 2001, 2002, 2003, 2004 Networks Associates Technology, Inc.
 * Copyright (c) 2005-2006 SPARTA, Inc.
 *
 * This software was developed by Robert Watson and Ilmar Habibulin for the
 * TrustedBSD Project.
 *
 * This software was developed for the FreeBSD Project in part by Network
 * Associates Laboratories, the Security Research Division of Network
 * Associates, Inc. under DARPA/SPAWAR contract N66001-01-C-8035 ("CBOSS"),
 * as part of the DARPA CHATS research program.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

/*-
 * Framework for extensible kernel access control.  This file contains
 * Kernel and userland interface to the framework, policy registration
 * and composition.  Per-object interfaces, controls, and labeling may be
 * found in src/sys/mac/.  Sample policies may be found in src/sys/mac*.
 */

#include <stdarg.h>
#include <string.h>
#include <security/mac_internal.h>
#include <security/mac_mach_internal.h>
#include <sys/param.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/vfs_context.h>
#include <sys/namei.h>
#include <bsd/bsm/audit.h>
#include <bsd/security/audit/audit.h>
#include <bsd/security/audit/audit_private.h>
#include <sys/file.h>
#include <sys/file_internal.h>
#include <sys/filedesc.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/kauth.h>
#include <sys/sysproto.h>

#include <mach/exception_types.h>
#include <mach/vm_types.h>
#include <mach/vm_prot.h>

#include <kern/kalloc.h>
#include <kern/sched_prim.h>
#include <kern/task.h>

#if CONFIG_MACF
#include <security/mac.h>
#include <security/mac_policy.h>
#include <security/mac_framework.h>
#include <security/mac_internal.h>
#include <security/mac_mach_internal.h>
#endif

#include <libkern/section_keywords.h>

/*
 * define MB_DEBUG to display run-time debugging information
 * #define MB_DEBUG 1
 */

#ifdef MB_DEBUG
#define DPRINTF(x)      printf x
#else
#define MB_DEBUG
#define DPRINTF(x)
#endif

#if CONFIG_MACF
SYSCTL_NODE(, OID_AUTO, security, CTLFLAG_RW | CTLFLAG_LOCKED, 0,
    "Security Controls");
SYSCTL_EXTENSIBLE_NODE(_security, OID_AUTO, mac, CTLFLAG_RW | CTLFLAG_LOCKED, 0,
    "TrustedBSD MAC policy controls");

/*
 * Declare that the kernel provides MAC support, version 1.  This permits
 * modules to refuse to be loaded if the necessary support isn't present,
 * even if it's pre-boot.
 */
#if 0
MODULE_VERSION(kernel_mac_support, 1);
#endif

#if MAC_MAX_SLOTS > 32
#error "MAC_MAX_SLOTS too large"
#endif

static unsigned int mac_max_slots = MAC_MAX_SLOTS;
static unsigned int mac_slot_offsets_free = (1 << MAC_MAX_SLOTS) - 1;
SYSCTL_UINT(_security_mac, OID_AUTO, max_slots, CTLFLAG_RD | CTLFLAG_LOCKED,
    &mac_max_slots, 0, "");

/*
 * Has the kernel started generating labeled objects yet?  All read/write
 * access to this variable is serialized during the boot process.  Following
 * the end of serialization, we don't update this flag; no locking.
 */
int     mac_late = 0;

/*
 * Flag to indicate whether or not we should allocate label storage for
 * new vnodes.  Since most dynamic policies we currently work with don't
 * rely on vnode labeling, try to avoid paying the cost of mtag allocation
 * unless specifically notified of interest.  One result of this is
 * that if a dynamically loaded policy requests vnode labels, it must
 * be able to deal with a NULL label being returned on any vnodes that
 * were already in flight when the policy was loaded.  Since the policy
 * already has to deal with uninitialized labels, this probably won't
 * be a problem.
 */
#if CONFIG_MACF_LAZY_VNODE_LABELS
unsigned int    mac_label_vnodes = 1;
#else
unsigned int    mac_label_vnodes = 0;
#endif /* CONFIG_MACF_LAZY_VNODE_LABELS */
SYSCTL_UINT(_security_mac, OID_AUTO, labelvnodes, SECURITY_MAC_CTLFLAGS
#if CONFIG_MACF_LAZY_VNODE_LABELS
    | CTLFLAG_RD
#endif
    , &mac_label_vnodes, 0, "Label all vnodes");

unsigned int mac_vnode_label_count = 0;
SYSCTL_UINT(_security_mac, OID_AUTO, vnode_label_count, SECURITY_MAC_CTLFLAGS | CTLFLAG_RD,
    &mac_vnode_label_count, 0, "Count of vnode labels");

unsigned int mac_device_enforce = 1;
SYSCTL_UINT(_security_mac, OID_AUTO, device_enforce, SECURITY_MAC_CTLFLAGS,
    &mac_device_enforce, 0, "Enforce MAC policy on device operations");

unsigned int    mac_pipe_enforce = 1;
SYSCTL_UINT(_security_mac, OID_AUTO, pipe_enforce, SECURITY_MAC_CTLFLAGS,
    &mac_pipe_enforce, 0, "Enforce MAC policy on pipe operations");

unsigned int    mac_posixsem_enforce = 1;
SYSCTL_UINT(_security_mac, OID_AUTO, posixsem_enforce, SECURITY_MAC_CTLFLAGS,
    &mac_posixsem_enforce, 0, "Enforce MAC policy on POSIX semaphores");

unsigned int mac_posixshm_enforce = 1;
SYSCTL_UINT(_security_mac, OID_AUTO, posixshm_enforce, SECURITY_MAC_CTLFLAGS,
    &mac_posixshm_enforce, 0, "Enforce MAC policy on Posix Shared Memory");

unsigned int    mac_proc_enforce = 1;
SYSCTL_UINT(_security_mac, OID_AUTO, proc_enforce, SECURITY_MAC_CTLFLAGS,
    &mac_proc_enforce, 0, "Enforce MAC policy on process operations");

unsigned int mac_socket_enforce = 1;
SYSCTL_UINT(_security_mac, OID_AUTO, socket_enforce, SECURITY_MAC_CTLFLAGS,
    &mac_socket_enforce, 0, "Enforce MAC policy on socket operations");

unsigned int    mac_system_enforce = 1;
SYSCTL_UINT(_security_mac, OID_AUTO, system_enforce, SECURITY_MAC_CTLFLAGS,
    &mac_system_enforce, 0, "Enforce MAC policy on system-wide interfaces");

unsigned int    mac_sysvmsg_enforce = 1;
SYSCTL_UINT(_security_mac, OID_AUTO, sysvmsg_enforce, SECURITY_MAC_CTLFLAGS,
    &mac_sysvmsg_enforce, 0, "Enforce MAC policy on System V IPC message queues");

unsigned int    mac_sysvsem_enforce = 1;
SYSCTL_UINT(_security_mac, OID_AUTO, sysvsem_enforce, SECURITY_MAC_CTLFLAGS,
    &mac_sysvsem_enforce, 0, "Enforce MAC policy on System V IPC semaphores");

unsigned int    mac_sysvshm_enforce = 1;
SYSCTL_INT(_security_mac, OID_AUTO, sysvshm_enforce, SECURITY_MAC_CTLFLAGS,
    &mac_sysvshm_enforce, 0, "Enforce MAC policy on System V Shared Memory");

unsigned int    mac_vm_enforce = 1;
SYSCTL_INT(_security_mac, OID_AUTO, vm_enforce, SECURITY_MAC_CTLFLAGS,
    &mac_vm_enforce, 0, "Enforce MAC policy on VM operations");

unsigned int    mac_vnode_enforce = 1;
SYSCTL_UINT(_security_mac, OID_AUTO, vnode_enforce, SECURITY_MAC_CTLFLAGS,
    &mac_vnode_enforce, 0, "Enforce MAC policy on vnode operations");

/*
 * mac_policy_list holds the list of policy modules.  Modules with a
 * handle lower than staticmax are considered "static" and cannot be
 * unloaded.  Such policies can be invoked without holding the busy count.
 *
 * Modules with a handle at or above the staticmax high water mark
 * are considered to be "dynamic" policies.  A busy count is maintained
 * for the list, stored in mac_policy_busy.  The busy count is protected
 * by mac_policy_mtx; the list may be modified only while the busy
 * count is 0, requiring that the lock be held to prevent new references
 * to the list from being acquired.  For almost all operations,
 * incrementing the busy count is sufficient to guarantee consistency,
 * as the list cannot be modified while the busy count is elevated.
 * For a few special operations involving a change to the list of
 * active policies, the mtx itself must be held.
 */
static LCK_GRP_DECLARE(mac_lck_grp, "MAC lock");
static LCK_MTX_DECLARE(mac_policy_mtx, &mac_lck_grp);

/*
 * Policy list array allocation chunk size. Each entry holds a pointer.
 */
#define MAC_POLICY_LIST_CHUNKSIZE 8

static int mac_policy_busy;

#if !XNU_TARGET_OS_OSX
SECURITY_READ_ONLY_LATE(mac_policy_list_t) mac_policy_list;
SECURITY_READ_ONLY_LATE(static struct mac_policy_list_element) mac_policy_static_entries[MAC_POLICY_LIST_CHUNKSIZE];
#else
mac_policy_list_t mac_policy_list;
#endif

/*
 * mac_label_element_list holds the master list of label namespaces for
 * all the policies. When a policy is loaded, each of it's label namespace
 * elements is added to the master list if not already present. When a
 * policy is unloaded, the namespace elements are removed if no other
 * policy is interested in that namespace element.
 */
struct mac_label_element_list_t mac_label_element_list;
struct mac_label_element_list_t mac_static_label_element_list;

static __inline void
mac_policy_grab_exclusive(void)
{
	lck_mtx_lock(&mac_policy_mtx);
	while (mac_policy_busy != 0) {
		lck_mtx_sleep(&mac_policy_mtx, LCK_SLEEP_UNLOCK,
		    (event_t)&mac_policy_busy, THREAD_UNINT);
		lck_mtx_lock(&mac_policy_mtx);
	}
}

static __inline void
mac_policy_release_exclusive(void)
{
	KASSERT(mac_policy_busy == 0,
	    ("mac_policy_release_exclusive(): not exclusive"));
	lck_mtx_unlock(&mac_policy_mtx);
	thread_wakeup((event_t) &mac_policy_busy);
}

void
mac_policy_list_busy(void)
{
	lck_mtx_lock(&mac_policy_mtx);
	mac_policy_busy++;
	lck_mtx_unlock(&mac_policy_mtx);
}

int
mac_policy_list_conditional_busy(void)
{
	int ret;

	if (mac_policy_list.numloaded <= mac_policy_list.staticmax) {
		return 0;
	}

	lck_mtx_lock(&mac_policy_mtx);
	if (mac_policy_list.numloaded > mac_policy_list.staticmax) {
		mac_policy_busy++;
		ret = 1;
	} else {
		ret = 0;
	}
	lck_mtx_unlock(&mac_policy_mtx);
	return ret;
}

void
mac_policy_list_unbusy(void)
{
	lck_mtx_lock(&mac_policy_mtx);
	mac_policy_busy--;
	KASSERT(mac_policy_busy >= 0, ("MAC_POLICY_LIST_LOCK"));
	if (mac_policy_busy == 0) {
		thread_wakeup(&mac_policy_busy);
	}
	lck_mtx_unlock(&mac_policy_mtx);
}

/*
 * Early pre-malloc MAC initialization, including appropriate SMP locks.
 */
void
mac_policy_init(void)
{
	mac_policy_list.numloaded = 0;
	mac_policy_list.max = MAC_POLICY_LIST_CHUNKSIZE;
	mac_policy_list.maxindex = 0;
	mac_policy_list.staticmax = 0;
	mac_policy_list.freehint = 0;
	mac_policy_list.chunks = 1;

#if !XNU_TARGET_OS_OSX
	mac_policy_list.entries = mac_policy_static_entries;
#else
	mac_policy_list.entries = kalloc_type(struct mac_policy_list_element,
	    MAC_POLICY_LIST_CHUNKSIZE, Z_WAITOK | Z_ZERO);
#endif

	SLIST_INIT(&mac_label_element_list);
	SLIST_INIT(&mac_static_label_element_list);
}

/* Function pointer set up for loading security extensions.
 * It is set to an actual function after OSlibkernInit()
 * has been called, and is set back to 0 by OSKextRemoveKextBootstrap()
 * after bsd_init().
 */
void (*load_security_extensions_function)(void) = 0;

/*
 * Init after early Mach startup, but before BSD
 */
void
mac_policy_initmach(void)
{
	/*
	 * For the purposes of modules that want to know if they were
	 * loaded "early", set the mac_late flag once we've processed
	 * modules either linked into the kernel, or loaded before the
	 * kernel startup.
	 */

	if (load_security_extensions_function) {
		load_security_extensions_function();
	}
	mac_late = 1;
}

/*
 * BSD startup.
 */
void
mac_policy_initbsd(void)
{
	struct mac_policy_conf *mpc;
	u_int i;

	printf("MAC Framework successfully initialized\n");

	/* Call bsd init functions of already loaded policies */

	/*
	 * Using the exclusive lock means no other framework entry
	 * points can proceed while initializations are running.
	 * This may not be necessary.
	 */
	mac_policy_grab_exclusive();

	for (i = 0; i <= mac_policy_list.maxindex; i++) {
		mpc = mac_get_mpc(i);
		if ((mpc != NULL) && (mpc->mpc_ops->mpo_policy_initbsd != NULL)) {
			(*(mpc->mpc_ops->mpo_policy_initbsd))(mpc);
		}
	}

	mac_policy_release_exclusive();
}

/*
 * After a policy has been loaded, add the label namespaces managed by the
 * policy to either the static or non-static label namespace list.
 * A namespace is added to the the list only if it is not already on one of
 * the lists.
 */
void
mac_policy_addto_labellist(mac_policy_handle_t handle, int static_entry)
{
	struct mac_label_element *mle, *mle_tmp;
	struct mac_label_listener *mll, *mll_tmp;
	struct mac_label_element_list_t *list;
	struct mac_policy_conf *mpc;
	const char *name, *name2;
	struct mac_label_element_list_t mles = SLIST_HEAD_INITIALIZER(mles);
	struct mac_label_listeners_t mlls = SLIST_HEAD_INITIALIZER(mlls);

	mpc = mac_get_mpc(handle);

	if (mpc->mpc_labelnames == NULL) {
		return;
	}

	if (mpc->mpc_labelname_count == 0) {
		return;
	}

	if (static_entry) {
		list = &mac_static_label_element_list;
	} else {
		list = &mac_label_element_list;
	}

	/*
	 * Before we grab the policy list lock, allocate enough memory
	 * to contain the potential new elements so we don't have to
	 * give up the lock, or allocate with the lock held.
	 */
	for (uint32_t idx = 0; idx < mpc->mpc_labelname_count; idx++) {
		mle = kalloc_type(struct mac_label_element, Z_WAITOK_ZERO_NOFAIL);
		SLIST_INSERT_HEAD(&mles, mle, mle_list);

		mll = kalloc_type(struct mac_label_listener, Z_WAITOK);
		SLIST_INSERT_HEAD(&mlls, mll, mll_list);
	}

	if (mac_late) {
		mac_policy_grab_exclusive();
	}
	for (uint32_t idx = 0; idx < mpc->mpc_labelname_count; idx++) {
		if (*(name = mpc->mpc_labelnames[idx]) == '?') {
			name++;
		}
		/*
		 * Check both label element lists and add to the
		 * appropriate list only if not already on a list.
		 */
		SLIST_FOREACH(mle, &mac_static_label_element_list, mle_list) {
			if (*(name2 = mle->mle_name) == '?') {
				name2++;
			}
			if (strcmp(name, name2) == 0) {
				break;
			}
		}
		if (mle == NULL) {
			SLIST_FOREACH(mle, &mac_label_element_list, mle_list) {
				if (*(name2 = mle->mle_name) == '?') {
					name2++;
				}
				if (strcmp(name, name2) == 0) {
					break;
				}
			}
		}
		if (mle == NULL) {
			mle = SLIST_FIRST(&mles);
			SLIST_REMOVE_HEAD(&mles, mle_list);
			strlcpy(mle->mle_name, mpc->mpc_labelnames[idx],
			    MAC_MAX_LABEL_ELEMENT_NAME);
			SLIST_INIT(&mle->mle_listeners);
			SLIST_INSERT_HEAD(list, mle, mle_list);
		}

		mll = SLIST_FIRST(&mlls);
		SLIST_REMOVE_HEAD(&mlls, mll_list);
		/* Add policy handler as a listener. */
		mll->mll_handle = handle;
		SLIST_INSERT_HEAD(&mle->mle_listeners, mll, mll_list);
	}
	if (mac_late) {
		mac_policy_release_exclusive();
	}

	SLIST_FOREACH_SAFE(mle, &mles, mle_list, mle_tmp) {
		kfree_type(struct mac_label_element, mle);
	}
	SLIST_FOREACH_SAFE(mll, &mlls, mll_list, mll_tmp) {
		kfree_type(struct mac_label_listener, mll);
	}
}

/*
 * After a policy has been unloaded, remove the label namespaces that the
 * the policy manages from the non-static list of namespaces.
 * The removal only takes place when no other policy is interested in the
 * namespace.
 *
 * Must be called with the policy exclusive lock held.
 */
void
mac_policy_removefrom_labellist(mac_policy_handle_t handle)
{
	struct mac_label_listener *mll, **mllp;
	struct mac_label_element *mle, **mlep;
	struct mac_policy_conf *mpc;

	mpc = mac_get_mpc(handle);

	if (mpc->mpc_labelnames == NULL) {
		return;
	}

	if (mpc->mpc_labelname_count == 0) {
		return;
	}

	/*
	 * Unregister policy as being interested in any label
	 * namespaces.  If no other policy is listening, remove
	 * that label element from the list.  Note that we only
	 * have to worry about the non-static list.
	 */
	SLIST_FOREACH_PREVPTR(mle, mlep, &mac_label_element_list, mle_list) {
		SLIST_FOREACH_PREVPTR(mll, mllp, &mle->mle_listeners, mll_list) {
			if (mll->mll_handle == handle) {
				*mllp = SLIST_NEXT(mll, mll_list);
				kfree_type(struct mac_label_listener, mll);
				if (SLIST_EMPTY(&mle->mle_listeners)) {
					*mlep = SLIST_NEXT(mle, mle_list);
					kfree_type(struct mac_label_element, mle);
				}
				return;
			}
		}
	}
}

/*
 * After the policy list has changed, walk the list to update any global
 * flags.
 */
static void
mac_policy_updateflags(void)
{
}

static __inline void
mac_policy_fixup_mmd_list(struct mac_module_data *new)
{
	struct mac_module_data *old;
	struct mac_module_data_element *ele, *aele;
	struct mac_module_data_list *arr, *dict;
	unsigned int i, j, k;

	old = new->base_addr;
	DPRINTF(("fixup_mmd: old %p new %p\n", old, new));
	for (i = 0; i < new->count; i++) {
		ele = &(new->data[i]);
		DPRINTF(("fixup_mmd: ele %p\n", ele));
		DPRINTF(("   key %p value %p\n", ele->key, ele->value));
		mmd_fixup_ele(old, new, ele); /* Fix up key/value ptrs.       */
		DPRINTF(("   key %p value %p\n", ele->key, ele->value));
		if (ele->value_type == MAC_DATA_TYPE_ARRAY) {
			arr = (struct mac_module_data_list *)ele->value;
			DPRINTF(("fixup_mmd: array @%p\n", arr));
			for (j = 0; j < arr->count; j++) {
				aele = &(arr->list[j]);
				DPRINTF(("fixup_mmd: aele %p\n", aele));
				DPRINTF(("   key %p value %p\n", aele->key, aele->value));
				mmd_fixup_ele(old, new, aele);
				DPRINTF(("   key %p value %p\n", aele->key, aele->value));
				if (arr->type == MAC_DATA_TYPE_DICT) {
					dict = (struct mac_module_data_list *)aele->value;
					DPRINTF(("fixup_mmd: dict @%p\n", dict));
					for (k = 0; k < dict->count; k++) {
						mmd_fixup_ele(old, new,
						    &(dict->list[k]));
					}
				}
			}
		}
	}
	new->base_addr = new;
}

int
mac_policy_register(struct mac_policy_conf *mpc, mac_policy_handle_t *handlep,
    void *xd)
{
#if XNU_TARGET_OS_OSX
	struct mac_policy_list_element *tmac_policy_list_element;
#endif
	int error, slot, static_entry = 0;
	u_int i;


	/*
	 * Some preliminary checks to make sure the policy's conf structure
	 * contains the required fields.
	 */
	if (mpc->mpc_name == NULL) {
		panic("policy's name is not set");
	}

	if (mpc->mpc_fullname == NULL) {
		panic("policy's full name is not set");
	}

	if (mpc->mpc_labelname_count > MAC_MAX_MANAGED_NAMESPACES) {
		panic("policy's managed label namespaces exceeds maximum");
	}

	if (mpc->mpc_ops == NULL) {
		panic("policy's OPs field is NULL");
	}

	error = 0;

	if (mac_late) {
		if (mpc->mpc_loadtime_flags & MPC_LOADTIME_FLAG_NOTLATE) {
			printf("Module %s does not support late loading.\n",
			    mpc->mpc_name);
			return EPERM;
		}
		mac_policy_grab_exclusive();
	}

	if (mac_policy_list.numloaded >= mac_policy_list.max) {
#if XNU_TARGET_OS_OSX
		/* allocate new policy list array, zero new chunk */
		tmac_policy_list_element =
		    kalloc_type(struct mac_policy_list_element,
		    MAC_POLICY_LIST_CHUNKSIZE * (mac_policy_list.chunks + 1),
		    Z_WAITOK | Z_ZERO);

		/* copy old entries into new list */
		memcpy(tmac_policy_list_element, mac_policy_list.entries,
		    sizeof(struct mac_policy_list_element) *
		    MAC_POLICY_LIST_CHUNKSIZE * mac_policy_list.chunks);

		/* free old array */
		kfree_type(struct mac_policy_list_element,
		    MAC_POLICY_LIST_CHUNKSIZE * mac_policy_list.chunks,
		    mac_policy_list.entries);

		mac_policy_list.entries = tmac_policy_list_element;

		/* Update maximums, etc */
		mac_policy_list.max += MAC_POLICY_LIST_CHUNKSIZE;
		mac_policy_list.chunks++;
#else
		printf("out of space in mac_policy_list.\n");
		return ENOMEM;
#endif /* XNU_TARGET_OS_OSX */
	}

	/* Check for policy with same name already loaded */
	for (i = 0; i <= mac_policy_list.maxindex; i++) {
		if (mac_policy_list.entries[i].mpc == NULL) {
			continue;
		}

		if (strcmp(mac_policy_list.entries[i].mpc->mpc_name,
		    mpc->mpc_name) == 0) {
			error = EEXIST;
			goto out;
		}
	}

	if (mpc->mpc_field_off != NULL) {
		slot = ffs(mac_slot_offsets_free);
		if (slot == 0) {
			error = ENOMEM;
			goto out;
		}
		slot--;
		mac_slot_offsets_free &= ~(1 << slot);
		*mpc->mpc_field_off = slot;
	}
	mpc->mpc_runtime_flags |= MPC_RUNTIME_FLAG_REGISTERED;

	if (xd) {
		struct mac_module_data *mmd = xd; /* module data from plist */

		/* Make a copy of the data. */
		mpc->mpc_data = (void *)kalloc_data(mmd->size, Z_WAITOK);
		if (mpc->mpc_data != NULL) {
			memcpy(mpc->mpc_data, mmd, mmd->size);

			/* Fix up pointers after copy. */
			mac_policy_fixup_mmd_list(mpc->mpc_data);
		}
	}

	/* Find the first free handle in the list (using our hint). */
	for (i = mac_policy_list.freehint; i < mac_policy_list.max; i++) {
		if (mac_policy_list.entries[i].mpc == NULL) {
			*handlep = i;
			mac_policy_list.freehint = ++i;
			break;
		}
	}

	/*
	 * If we are loading a MAC module before the framework has
	 * finished initializing or the module is not unloadable and
	 * we can place its handle adjacent to the last static entry,
	 * bump the static policy high water mark.
	 * Static policies can get by with weaker locking requirements.
	 */
	if (!mac_late ||
	    ((mpc->mpc_loadtime_flags & MPC_LOADTIME_FLAG_UNLOADOK) == 0 &&
	    *handlep == mac_policy_list.staticmax)) {
		static_entry = 1;
		mac_policy_list.staticmax++;
	}

	mac_policy_list.entries[*handlep].mpc = mpc;

	/* Update counters, etc */
	if (*handlep > mac_policy_list.maxindex) {
		mac_policy_list.maxindex = *handlep;
	}
	mac_policy_list.numloaded++;

	/* Per-policy initialization. */
	printf("calling mpo_policy_init for %s\n", mpc->mpc_name);
	if (mpc->mpc_ops->mpo_policy_init != NULL) {
		(*(mpc->mpc_ops->mpo_policy_init))(mpc);
	}

	if (mac_late && mpc->mpc_ops->mpo_policy_initbsd != NULL) {
		printf("calling mpo_policy_initbsd for %s\n", mpc->mpc_name);
		(*(mpc->mpc_ops->mpo_policy_initbsd))(mpc);
	}

	mac_policy_updateflags();

	if (mac_late) {
		mac_policy_release_exclusive();
	}

	mac_policy_addto_labellist(*handlep, static_entry);

	printf("Security policy loaded: %s (%s)\n", mpc->mpc_fullname,
	    mpc->mpc_name);

	return 0;

out:
	if (mac_late) {
		mac_policy_release_exclusive();
	}

	return error;
}

int
mac_policy_unregister(mac_policy_handle_t handle)
{
	struct mac_policy_conf *mpc;

	/*
	 * If we fail the load, we may get a request to unload.  Check
	 * to see if we did the run-time registration, and if not,
	 * silently succeed.
	 */
	mac_policy_grab_exclusive();
	mpc = mac_get_mpc(handle);
	if ((mpc->mpc_runtime_flags & MPC_RUNTIME_FLAG_REGISTERED) == 0) {
		mac_policy_release_exclusive();
		return 0;
	}

#if 0
	/*
	 * Don't allow unloading modules with private data.
	 */
	if (mpc->mpc_field_off != NULL) {
		MAC_POLICY_LIST_UNLOCK();
		return EBUSY;
	}
#endif
	/*
	 * Only allow the unload to proceed if the module is unloadable
	 * by its own definition.
	 */
	if ((mpc->mpc_loadtime_flags & MPC_LOADTIME_FLAG_UNLOADOK) == 0) {
		mac_policy_release_exclusive();
		return EBUSY;
	}

	mac_policy_removefrom_labellist(handle);

	mac_get_mpc(handle) = NULL;
	if (handle < mac_policy_list.freehint &&
	    handle >= mac_policy_list.staticmax) {
		mac_policy_list.freehint = handle;
	}

	if (handle == mac_policy_list.maxindex) {
		mac_policy_list.maxindex--;
	}

	mac_policy_list.numloaded--;
	if (mpc->mpc_field_off != NULL) {
		mac_slot_offsets_free |= (1 << *mpc->mpc_field_off);
	}

	if (mpc->mpc_ops->mpo_policy_destroy != NULL) {
		(*(mpc->mpc_ops->mpo_policy_destroy))(mpc);
	}

	mpc->mpc_runtime_flags &= ~MPC_RUNTIME_FLAG_REGISTERED;
	mac_policy_updateflags();

	mac_policy_release_exclusive();

	if (mpc->mpc_data) {
		struct mac_module_data *mmd = mpc->mpc_data;
		__typed_allocators_ignore(kfree_data(mmd, mmd->size)); // rdar://87952845
		mpc->mpc_data = NULL;
	}

	printf("Security policy unload: %s (%s)\n", mpc->mpc_fullname,
	    mpc->mpc_name);

	return 0;
}

/*
 * Define an error value precedence, and given two arguments, selects the
 * value with the higher precedence.
 */
int
mac_error_select(int error1, int error2)
{
	/* Certain decision-making errors take top priority. */
	if (error1 == EDEADLK || error2 == EDEADLK) {
		return EDEADLK;
	}

	/* Invalid arguments should be reported where possible. */
	if (error1 == EINVAL || error2 == EINVAL) {
		return EINVAL;
	}

	/* Precedence goes to "visibility", with both process and file. */
	if (error1 == ESRCH || error2 == ESRCH) {
		return ESRCH;
	}

	if (error1 == ENOENT || error2 == ENOENT) {
		return ENOENT;
	}

	/* Precedence goes to DAC/MAC protections. */
	if (error1 == EACCES || error2 == EACCES) {
		return EACCES;
	}

	/* Precedence goes to privilege. */
	if (error1 == EPERM || error2 == EPERM) {
		return EPERM;
	}

	/* Precedence goes to error over success; otherwise, arbitrary. */
	if (error1 != 0) {
		return error1;
	}
	return error2;
}

int
mac_check_structmac_consistent(struct user_mac *mac)
{
	if (mac->m_buflen > MAC_MAX_LABEL_BUF_LEN || mac->m_buflen == 0) {
		return EINVAL;
	}

	return 0;
}

/*
 * Get the external forms of labels from all policies, for a single
 * label namespace or "*" for all namespaces.  Returns ENOENT if no policy
 * is registered for the namespace, unless the namespace begins with a '?'.
 */
static int
mac_label_externalize(size_t mpo_externalize_off, struct label *label,
    const char *element, struct sbuf *sb)
{
	struct mac_policy_conf *mpc;
	struct mac_label_listener *mll;
	struct mac_label_element *mle;
	struct mac_label_element_list_t *element_list;
	const char *name;
	int (*mpo_externalize)(struct label *, char *, struct sbuf *);
	int all_labels = 0, ignorenotfound = 0, error = 0, busy = FALSE;
	int sb_pos;
	unsigned int count = 0;

	if (element[0] == '?') {
		element++;
		ignorenotfound = 1;
	} else if (element[0] == '*' && element[1] == '\0') {
		all_labels = 1;
	}

	element_list = &mac_static_label_element_list;
element_loop:
	SLIST_FOREACH(mle, element_list, mle_list) {
		name = mle->mle_name;
		if (all_labels) {
			if (*name == '?') {
				continue;
			}
		} else {
			if (*name == '?') {
				name++;
			}
			if (strcmp(name, element) != 0) {
				continue;
			}
		}
		SLIST_FOREACH(mll, &mle->mle_listeners, mll_list) {
			mpc = mac_policy_list.entries[mll->mll_handle].mpc;
			if (mpc == NULL) {
				continue;
			}
			mpo_externalize = *(const typeof(mpo_externalize) *)
			    ((const char *)mpc->mpc_ops + mpo_externalize_off);
			if (mpo_externalize == NULL) {
				continue;
			}
			sb_pos = sbuf_len(sb);
			error = sbuf_printf(sb, "%s/", name);
			if (error) {
				goto done;
			}
			error = mpo_externalize(label, mle->mle_name, sb);
			if (error) {
				if (error != ENOENT) {
					goto done;
				}
				/*
				 * If a policy doesn't have a label to
				 * externalize it returns ENOENT.  This
				 * may occur for policies that support
				 * multiple label elements for some
				 * (but not all) object types.
				 */
				sbuf_setpos(sb, sb_pos);
				error = 0;
				continue;
			}
			error = sbuf_putc(sb, ',');
			if (error) {
				goto done;
			}
			count++;
		}
	}
	/* If there are dynamic policies present, check their elements too. */
	if (!busy && mac_policy_list_conditional_busy() == 1) {
		element_list = &mac_label_element_list;
		busy = TRUE;
		goto element_loop;
	}
done:
	if (busy) {
		mac_policy_list_unbusy();
	}
	if (!error && count == 0) {
		if (!all_labels && !ignorenotfound) {
			error = ENOENT; /* XXX: ENOLABEL? */
		}
	}
	return error;
}

/*
 * Get the external forms of labels from all policies, for all label
 * namespaces contained in a list.
 *
 * XXX This may be leaking an sbuf.
 */
int
mac_externalize(size_t mpo_externalize_off, struct label *label,
    const char *elementlist, char *outbuf, size_t outbuflen)
{
	char *element;
	char *scratch_base;
	char *scratch;
	struct sbuf sb;
	int error = 0, len;
	size_t buf_len = strlen(elementlist) + 1;

	/* allocate a scratch buffer the size of the string */
	scratch_base = kalloc_data(buf_len, Z_WAITOK);
	if (scratch_base == NULL) {
		error = ENOMEM;
		goto out;
	}

	/* copy the elementlist to the scratch buffer */
	strlcpy(scratch_base, elementlist, buf_len);

	/*
	 * set up a temporary pointer that can be used to iterate the
	 * scratch buffer without losing the allocation address
	 */
	scratch = scratch_base;

	/*
	 * initialize an sbuf mapping over the output buffer (or newly-allocated internal buffer, if
	 * outbuf is NULL), up to sbuf's limit of INT_MAX.
	 */
	if (outbuflen > INT_MAX) {
		outbuflen = INT_MAX;
	}
	if (sbuf_new(&sb, outbuf, (int)outbuflen, SBUF_FIXEDLEN) == NULL) {
		/* could not allocate interior buffer */
		error = ENOMEM;
		goto out;
	}
	/* iterate the scratch buffer; NOTE: buffer contents modified! */
	while ((element = strsep(&scratch, ",")) != NULL) {
		error = mac_label_externalize(mpo_externalize_off, label,
		    element, &sb);
		if (error) {
			break;
		}
	}
	if ((len = sbuf_len(&sb)) > 0) {
		sbuf_setpos(&sb, len - 1);      /* trim trailing comma */
	}
	sbuf_finish(&sb);

out:
	if (scratch_base != NULL) {
		kfree_data(scratch_base, buf_len);
	}

	return error;
}

/*
 * Have all policies set the internal form of a label, for a single
 * label namespace.
 */
static int
mac_label_internalize(size_t mpo_internalize_off, struct label *label,
    char *element_name, char *element_data)
{
	struct mac_policy_conf *mpc;
	struct mac_label_listener *mll;
	struct mac_label_element *mle;
	struct mac_label_element_list_t *element_list;
	int (*mpo_internalize)(struct label *, char *, char *);
	int error = 0, busy = FALSE;
	unsigned int count = 0;
	const char *name;

	element_list = &mac_static_label_element_list;
element_loop:
	SLIST_FOREACH(mle, element_list, mle_list) {
		if (*(name = mle->mle_name) == '?') {
			name++;
		}
		if (strcmp(element_name, name) != 0) {
			continue;
		}
		SLIST_FOREACH(mll, &mle->mle_listeners, mll_list) {
			mpc = mac_policy_list.entries[mll->mll_handle].mpc;
			if (mpc == NULL) {
				continue;
			}
			mpo_internalize = *(const typeof(mpo_internalize) *)
			    ((const char *)mpc->mpc_ops + mpo_internalize_off);
			if (mpo_internalize == NULL) {
				continue;
			}
			error = mpo_internalize(label, element_name,
			    element_data);
			if (error) {
				goto done;
			}
			count++;
		}
	}
	/* If there are dynamic policies present, check their elements too. */
	if (!busy && mac_policy_list_conditional_busy() == 1) {
		element_list = &mac_label_element_list;
		busy = TRUE;
		goto element_loop;
	}
done:
	if (busy) {
		mac_policy_list_unbusy();
	}
	if (!error && count == 0) {
		error = ENOPOLICY;
	}
	return error;
}

int
mac_internalize(size_t mpo_internalize_off, struct label *label,
    char *textlabels)
{
	char *element_name, *element_data;
	int error = 0;

	while (!error && (element_name = strsep(&textlabels, ",")) != NULL) {
		element_data = strchr(element_name, '/');
		if (element_data == NULL) {
			error = EINVAL;
			break;
		}
		*element_data++ = '\0';
		error = mac_label_internalize(mpo_internalize_off, label,
		    element_name, element_data);
	}
	return error;
}

static int
user_mac_copyin(struct proc *p, user_addr_t mac_p, struct user_mac *mac)
{
	int error;

	if (IS_64BIT_PROCESS(p)) {
		struct user64_mac mac64;
		if ((error = copyin(mac_p, &mac64, sizeof(mac64)))) {
			return error;
		}

		mac->m_buflen = mac64.m_buflen;
		mac->m_string = mac64.m_string;
	} else {
		struct user32_mac mac32;
		if ((error = copyin(mac_p, &mac32, sizeof(mac32)))) {
			return error;
		}

		mac->m_buflen = mac32.m_buflen;
		mac->m_string = mac32.m_string;
	}

	return mac_check_structmac_consistent(mac);
}

int
mac_do_get(struct proc *p, user_addr_t mac_p, mac_getter_t getter)
{
	struct user_mac mac;
	char *input;
	char *output;
	size_t len;
	size_t ulen;
	int error;

	if ((error = user_mac_copyin(p, mac_p, &mac))) {
		return error;
	}

	len = mac.m_buflen;
	input = kalloc_data(len, Z_WAITOK);
	if ((error = copyinstr(mac.m_string, input, len, &ulen))) {
		kfree_data(input, len);
		return error;
	}

	AUDIT_ARG(mac_string, input);

	output = kalloc_data(len, Z_WAITOK | Z_ZERO);

	error = getter(input, output, len);
	if (error == 0) {
		/* mac_check_structmac_consistent => len > 0 */
		output[len - 1] = '\0';
		error = copyout(output, mac.m_string, strlen(output) + 1);
	}

	kfree_data(output, len);
	kfree_data(input, len);
	return error;
}

int
mac_do_set(struct proc *p, user_addr_t mac_p, mac_setter_t setter)
{
	struct user_mac mac;
	char *input;
	size_t len;
	size_t ulen;
	int error;

	if ((error = user_mac_copyin(p, mac_p, &mac))) {
		return error;
	}

	len = mac.m_buflen;
	input = kalloc_data(len, Z_WAITOK);
	if ((error = copyinstr(mac.m_string, input, len, &ulen))) {
		kfree_data(input, len);
		return error;
	}

	AUDIT_ARG(mac_string, input);

	error = setter(input, len);

	kfree_data(input, len);
	return error;
}

/* system calls */

int
__mac_get_pid(struct proc *p, struct __mac_get_pid_args *uap, int *ret __unused)
{
	return mac_do_get(p, uap->mac_p,
	           ^(char *input, char *output, size_t len) {
		struct ucred *tcred;
		int error;

		AUDIT_ARG(pid, uap->pid);

		tcred = kauth_cred_proc_ref_for_pid(uap->pid);
		if (tcred == NOCRED) {
		        return ESRCH;
		}

		error = mac_cred_label_externalize(mac_cred_label(tcred),
		input, output, len, M_WAITOK);

		kauth_cred_unref(&tcred);
		return error;
	});
}

int
__mac_get_proc(proc_t p, struct __mac_get_proc_args *uap, int *ret __unused)
{
	return mac_do_get(p, uap->mac_p,
	           ^(char *input, char *output, size_t len) {
		struct label *label;

		label = mac_cred_label(kauth_cred_get());

		return mac_cred_label_externalize(label, input, output, len, M_WAITOK);
	});
}

int
__mac_set_proc(proc_t p, struct __mac_set_proc_args *uap, int *ret __unused)
{
	return mac_do_set(p, uap->mac_p,
	           ^(char *input, __unused size_t len) {
		struct label *intlabel;
		int error;

		intlabel = mac_cred_label_alloc();
		if ((error = mac_cred_label_internalize(intlabel, input))) {
		        goto out;
		}

		if ((error = mac_cred_check_label_update(kauth_cred_get(), intlabel))) {
		        goto out;
		}

		error = kauth_proc_label_update(p, intlabel);

		out:
		mac_cred_label_free(intlabel);
		return error;
	});
}

int
__mac_get_fd(proc_t p, struct __mac_get_fd_args *uap, int *ret __unused)
{
	return mac_do_get(p, uap->mac_p,
	           ^(char *input, char *output, size_t len) {
		struct fileproc *fp;
		struct vnode *vp;
		int error;
		struct label *intlabel;

		AUDIT_ARG(fd, uap->fd);

		if ((error = fp_lookup(p, uap->fd, &fp, 0))) {
		        return error;
		}

		error = mac_file_check_get(kauth_cred_get(), fp->fp_glob, input, len);
		if (error) {
		        fp_drop(p, uap->fd, fp, 0);
		        return error;
		}

		switch (FILEGLOB_DTYPE(fp->fp_glob)) {
		case DTYPE_VNODE:
			intlabel = mac_vnode_label_alloc(NULL);
			if (intlabel == NULL) {
			        error = ENOMEM;
			        break;
			}
			vp = (struct vnode *)fp_get_data(fp);
			error = vnode_getwithref(vp);
			if (error == 0) {
			        mac_vnode_label_copy(mac_vnode_label(vp), intlabel);
			        error = mac_vnode_label_externalize(intlabel,
			        input, output, len, M_WAITOK);
			        vnode_put(vp);
			}
			mac_vnode_label_free(intlabel);
			break;
		case DTYPE_SOCKET:
		case DTYPE_PSXSHM:
		case DTYPE_PSXSEM:
		case DTYPE_PIPE:
		case DTYPE_KQUEUE:
		case DTYPE_FSEVENTS:
		case DTYPE_ATALK:
		case DTYPE_NETPOLICY:
		case DTYPE_CHANNEL:
		case DTYPE_NEXUS:
		default:
			error = ENOSYS;           // only sockets/vnodes so far
			break;
		}
		fp_drop(p, uap->fd, fp, 0);
		return error;
	});
}

static int
mac_get_filelink(proc_t p, user_addr_t mac_p, user_addr_t path_p, int follow)
{
	return mac_do_get(p, mac_p,
	           ^(char *input, char *output, size_t len) {
		struct vnode *vp;
		struct nameidata nd;
		struct label *intlabel;
		int error;

		NDINIT(&nd, LOOKUP, OP_LOOKUP,
		LOCKLEAF | (follow ? FOLLOW : NOFOLLOW) | AUDITVNPATH1,
		UIO_USERSPACE, path_p,
		vfs_context_current());
		if ((error = namei(&nd))) {
		        return error;
		}
		vp = nd.ni_vp;

		nameidone(&nd);

		intlabel = mac_vnode_label_alloc(NULL);
		mac_vnode_label_copy(mac_vnode_label(vp), intlabel);
		error = mac_vnode_label_externalize(intlabel, input, output,
		len, M_WAITOK);
		mac_vnode_label_free(intlabel);

		vnode_put(vp);
		return error;
	});
}

int
__mac_get_file(proc_t p, struct __mac_get_file_args *uap,
    int *ret __unused)
{
	return mac_get_filelink(p, uap->mac_p, uap->path_p, 1);
}

int
__mac_get_link(proc_t p, struct __mac_get_link_args *uap,
    int *ret __unused)
{
	return mac_get_filelink(p, uap->mac_p, uap->path_p, 0);
}

int
__mac_set_fd(proc_t p, struct __mac_set_fd_args *uap, int *ret __unused)
{
	return mac_do_set(p, uap->mac_p,
	           ^(char *input, size_t len) {
		struct fileproc *fp;
		struct vfs_context *ctx = vfs_context_current();
		int error;
		struct label *intlabel;
		struct vnode *vp;

		AUDIT_ARG(fd, uap->fd);

		if ((error = fp_lookup(p, uap->fd, &fp, 0))) {
		        return error;
		}

		error = mac_file_check_set(vfs_context_ucred(ctx), fp->fp_glob, input, len);
		if (error) {
		        fp_drop(p, uap->fd, fp, 0);
		        return error;
		}

		switch (FILEGLOB_DTYPE(fp->fp_glob)) {
		case DTYPE_VNODE:
			if (mac_label_vnodes == 0) {
			        error = ENOSYS;
			        break;
			}

			intlabel = mac_vnode_label_alloc(NULL);

			error = mac_vnode_label_internalize(intlabel, input);
			if (error) {
			        mac_vnode_label_free(intlabel);
			        break;
			}

			vp = (struct vnode *)fp_get_data(fp);

			error = vnode_getwithref(vp);
			if (error == 0) {
			        error = vn_setlabel(vp, intlabel, ctx);
			        vnode_put(vp);
			}
			mac_vnode_label_free(intlabel);
			break;

		case DTYPE_SOCKET:
		case DTYPE_PSXSHM:
		case DTYPE_PSXSEM:
		case DTYPE_PIPE:
		case DTYPE_KQUEUE:
		case DTYPE_FSEVENTS:
		case DTYPE_ATALK:
		case DTYPE_NETPOLICY:
		case DTYPE_CHANNEL:
		case DTYPE_NEXUS:
		default:
			error = ENOSYS;          // only sockets/vnodes so far
			break;
		}

		fp_drop(p, uap->fd, fp, 0);
		return error;
	});
}

static int
mac_set_filelink(proc_t p, user_addr_t mac_p, user_addr_t path_p,
    int follow)
{
	return mac_do_set(p, mac_p,
	           ^(char *input, __unused size_t len) {
		struct vnode *vp;
		struct vfs_context *ctx = vfs_context_current();
		struct label *intlabel;
		struct nameidata nd;
		int error;

		if (mac_label_vnodes == 0) {
		        return ENOSYS;
		}

		intlabel = mac_vnode_label_alloc(NULL);
		error = mac_vnode_label_internalize(intlabel, input);
		if (error) {
		        mac_vnode_label_free(intlabel);
		        return error;
		}

		NDINIT(&nd, LOOKUP, OP_LOOKUP,
		LOCKLEAF | (follow ? FOLLOW : NOFOLLOW) | AUDITVNPATH1,
		UIO_USERSPACE, path_p, ctx);
		error = namei(&nd);
		if (error) {
		        mac_vnode_label_free(intlabel);
		        return error;
		}
		vp = nd.ni_vp;

		nameidone(&nd);

		error = vn_setlabel(vp, intlabel, ctx);
		vnode_put(vp);
		mac_vnode_label_free(intlabel);

		return error;
	});
}

int
__mac_set_file(proc_t p, struct __mac_set_file_args *uap,
    int *ret __unused)
{
	return mac_set_filelink(p, uap->mac_p, uap->path_p, 1);
}

int
__mac_set_link(proc_t p, struct __mac_set_link_args *uap,
    int *ret __unused)
{
	return mac_set_filelink(p, uap->mac_p, uap->path_p, 0);
}

static int
mac_proc_check_mac_syscall(proc_t p, const char *target, int callnum)
{
	int error;

#if SECURITY_MAC_CHECK_ENFORCE
	/* 21167099 - only check if we allow write */
	if (!mac_proc_enforce) {
		return 0;
	}
#endif

	MAC_CHECK(proc_check_syscall_mac, p, target, callnum);

	return error;
}

/*
 * __mac_syscall: Perform a MAC policy system call
 *
 * Parameters:    p                       Process calling this routine
 *                uap                     User argument descriptor (see below)
 *                retv                    (Unused)
 *
 * Indirect:      uap->policy             Name of target MAC policy
 *                uap->call               MAC policy-specific system call to perform
 *                uap->arg                MAC policy-specific system call arguments
 *
 * Returns:        0                      Success
 *                !0                      Not success
 *
 */
int
__mac_syscall(proc_t p, struct __mac_syscall_args *uap, int *retv __unused)
{
	struct mac_policy_conf *mpc;
	char target[MAC_MAX_POLICY_NAME];
	int error;
	u_int i;
	size_t ulen;

	error = copyinstr(uap->policy, target, sizeof(target), &ulen);
	if (error) {
		return error;
	}
	AUDIT_ARG(value32, uap->call);
	AUDIT_ARG(mac_string, target);

	error = mac_proc_check_mac_syscall(p, target, uap->call);
	if (error) {
		return error;
	}

	error = ENOPOLICY;

	for (i = 0; i < mac_policy_list.staticmax; i++) {
		mpc = mac_policy_list.entries[i].mpc;
		if (mpc == NULL) {
			continue;
		}

		if (strcmp(mpc->mpc_name, target) == 0 &&
		    mpc->mpc_ops->mpo_policy_syscall != NULL) {
			error = mpc->mpc_ops->mpo_policy_syscall(p,
			    uap->call, uap->arg);
			goto done;
		}
	}
	if (mac_policy_list_conditional_busy() != 0) {
		for (; i <= mac_policy_list.maxindex; i++) {
			mpc = mac_policy_list.entries[i].mpc;
			if (mpc == NULL) {
				continue;
			}

			if (strcmp(mpc->mpc_name, target) == 0 &&
			    mpc->mpc_ops->mpo_policy_syscall != NULL) {
				error = mpc->mpc_ops->mpo_policy_syscall(p,
				    uap->call, uap->arg);
				break;
			}
		}
		mac_policy_list_unbusy();
	}

done:
	return error;
}

int
mac_mount_label_get(struct mount *mp, user_addr_t mac_p)
{
	return mac_do_get(current_proc(), mac_p,
	           ^(char *input, char *output, size_t len) {
		return mac_mount_label_externalize(mac_mount_label(mp), input,
		output, len);
	});
}

/*
 * __mac_get_mount: Get mount point label information for a given pathname
 *
 * Parameters:    p                        (ignored)
 *                uap                      User argument descriptor (see below)
 *                ret                      (ignored)
 *
 * Indirect:      uap->path                Pathname
 *                uap->mac_p               MAC info
 *
 * Returns:        0                       Success
 *                !0                       Not success
 */
int
__mac_get_mount(proc_t p __unused, struct __mac_get_mount_args *uap,
    int *ret __unused)
{
	struct nameidata nd;
	struct vfs_context *ctx = vfs_context_current();
	struct mount *mp;
	int error;

	NDINIT(&nd, LOOKUP, OP_LOOKUP, FOLLOW | AUDITVNPATH1,
	    UIO_USERSPACE, uap->path, ctx);
	error = namei(&nd);
	if (error) {
		return error;
	}
	mp = nd.ni_vp->v_mount;
	mount_ref(mp, 0);
	vnode_put(nd.ni_vp);
	nameidone(&nd);

	error = mac_mount_label_get(mp, uap->mac_p);
	mount_drop(mp, 0);
	return error;
}

/*
 * mac_schedule_userret()
 *
 * Schedule a callback to the mpo_thread_userret hook. The mpo_thread_userret
 * hook is called just before the thread exit from the kernel in ast_taken().
 *
 * Returns:	 0		Success
 *              !0		Not successful
 */
int
mac_schedule_userret(void)
{
	act_set_astmacf(current_thread());
	return 0;
}

/*
 * mac_do_machexc()
 *
 * Do a Mach exception.  This should only be done in the mpo_thread_userret
 * callback.
 *
 * params:	code		exception code
 *              subcode		exception subcode
 *              flags		flags:
 *                              MAC_DOEXCF_TRACED  Only do exception if being
 *                                                 ptrace()'ed.
 *
 *
 * Returns:	 0		Success
 *              !0		Not successful
 */
int
mac_do_machexc(int64_t code, int64_t subcode, uint32_t flags)
{
	mach_exception_data_type_t  codes[EXCEPTION_CODE_MAX];
	proc_t p = current_proc();

	/* Only allow execption codes in MACF's reserved range. */
	if ((code < EXC_MACF_MIN) || (code > EXC_MACF_MAX)) {
		return 1;
	}

	if (flags & MAC_DOEXCF_TRACED &&
	    !(p->p_lflag & P_LTRACED && (p->p_lflag & P_LPPWAIT) == 0)) {
		return 0;
	}


	/* Send the Mach exception */
	codes[0] = (mach_exception_data_type_t)code;
	codes[1] = (mach_exception_data_type_t)subcode;

	return bsd_exception(EXC_SOFTWARE, codes, 2) != KERN_SUCCESS;
}

#else /* MAC */

void (*load_security_extensions_function)(void) = 0;

struct sysctl_oid_list sysctl__security_mac_children;

int
mac_policy_register(struct mac_policy_conf *mpc __unused,
    mac_policy_handle_t *handlep __unused, void *xd __unused)
{
	return 0;
}

int
mac_policy_unregister(mac_policy_handle_t handle __unused)
{
	return 0;
}

int
mac_audit_text(char *text __unused, mac_policy_handle_t handle __unused)
{
	return 0;
}

int
mac_vnop_setxattr(struct vnode *vp __unused, const char *name __unused, char *buf __unused, size_t len __unused)
{
	return ENOENT;
}

int
mac_vnop_getxattr(struct vnode *vp __unused, const char *name __unused,
    char *buf __unused, size_t len __unused, size_t *attrlen __unused)
{
	return ENOENT;
}

int
mac_vnop_removexattr(struct vnode *vp __unused, const char *name __unused)
{
	return ENOENT;
}

int
mac_file_setxattr(struct fileglob *fg __unused, const char *name __unused, char *buf __unused, size_t len __unused)
{
	return ENOENT;
}

int
mac_file_getxattr(struct fileglob *fg __unused, const char *name __unused,
    char *buf __unused, size_t len __unused, size_t *attrlen __unused)
{
	return ENOENT;
}

int
mac_file_removexattr(struct fileglob *fg __unused, const char *name __unused)
{
	return ENOENT;
}

intptr_t
mac_label_get(struct label *l __unused, int slot __unused)
{
	return 0;
}

void
mac_label_set(struct label *l __unused, int slot __unused, intptr_t v __unused)
{
	return;
}

int mac_iokit_check_hid_control(kauth_cred_t cred __unused);
int
mac_iokit_check_hid_control(kauth_cred_t cred __unused)
{
	return 0;
}

int mac_vnode_check_open(vfs_context_t ctx, struct vnode *vp, int acc_mode);
int
mac_vnode_check_open(vfs_context_t ctx __unused, struct vnode *vp __unused, int acc_mode __unused)
{
	return 0;
}

int mac_mount_check_snapshot_mount(vfs_context_t ctx, struct vnode *rvp, struct vnode *vp, struct componentname *cnp,
    const char *name, const char *vfc_name);
int
mac_mount_check_snapshot_mount(vfs_context_t ctx __unused, struct vnode *rvp __unused, struct vnode *vp __unused,
    struct componentname *cnp __unused, const char *name __unused, const char *vfc_name __unused)
{
	return 0;
}

int mac_vnode_check_trigger_resolve(vfs_context_t ctx __unused, struct vnode *dvp __unused, struct componentname *cnp __unused);
int
mac_vnode_check_trigger_resolve(vfs_context_t ctx __unused, struct vnode *dvp __unused, struct componentname *cnp __unused)
{
	return 0;
}

#endif /* !MAC */
