/*
 * @File: mcManager.c
 *
 * @Abstract: Multicast manager
 *
 * @Notes:
 *
 * Copyright (c) 2014-2015, 2017, 2019-2020 Qualcomm Technologies, Inc.
 *
 * All Rights Reserved.
 * Confidential and Proprietary - Qualcomm Technologies, Inc.
 *
 * 2014-2015, 2017 Qualcomm Atheros, Inc.
 *
 * All Rights Reserved.
 * Qualcomm Atheros Confidential and Proprietary.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <asm/types.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <net/if.h>
#include <dbg.h>
#include <bufrd.h>
#include "module.h"
#include "profile.h"
#include "internal.h"
#include "mcnl.h"
#include "hashmap.h"

#include "mcManager.h"
#include "qassert.h"
#ifdef MCS_MODULE_PLC
#include "plcManager.h"
#include "service_message.h"
#endif
#ifdef MCS_MODULE_WLAN
#include "wlanManager.h"
#include "mcfwdtblwlan5g.h"
#include "mcfwdtblwlan2g.h"
#else
#include <net/ethernet.h>
#endif
#include "mcfwdtbleswitch.h"

#ifdef MCS_MODULE_PLC
#define mcManagerSetPLCMCDelay         2
#define mcManagerEventPLCSnoopingFail  0x01
#define mcManagerEventPLCTableFail     0x02
#endif

#define MCM_DEF_MDB_ENTRY_NUM          64
#define MCD_DEF_NL_RD_BUF_LEN          (NLMSG_HDRLEN + MC_MSG_HDRLEN + sizeof(struct __mc_mdb_entry)*MCM_DEF_MDB_ENTRY_NUM + 16)
static int mcManagerReadBufLen = MCD_DEF_NL_RD_BUF_LEN;
static int mcManagerNumMDBEntry = MCM_DEF_MDB_ENTRY_NUM;

struct mcManagerEncapTable {
	int32_t entryCnt;
	struct __mc_encaptbl_entry entry[MC_DEF_GROUP_MAX];
};

struct mcManagerFloodTable {
	int32_t entryCnt;
	struct __mc_floodtbl_entry entry[MC_DEF_GROUP_MAX];
};

struct mcManagerIFTable {
	int32_t entryCnt;
	struct __mc_iftbl_entry entry[MC_DEF_GROUP_MAX];
};

struct mcManagerIFNode {
	struct hmap_node node;
	interface_t interface;
	interfaceSync_e sync;
	struct mcManagerIFTable *table;
	struct mcManagerIFTable *temptable;
};

struct mcManagerConfig_t {
	u_int32_t PathTransitionMethod;
	u_int32_t LoadBalancingSeamless;
	u_int32_t ConstrainTCPMedium;
};

struct mcManagerSnooper {
	struct hmap_node node;
	struct mcManagerEncapTable *encapT;	/*encap table*/
	struct mcManagerFloodTable *floodT;	/*flood table*/
	struct hmap ifTableMap;			/*container of each wifi/switch interface*/
};
static struct mcManagerState_t {
	u_int32_t IsInit;		/* overall initialization done */
	int32_t MCNLSock;		/* netlink socket */
	struct bufrd MCReadBuf;		/* for reading from - netlink multicast */
	struct dbgModule *DebugModule;	/* debug message context */
	struct mcManagerConfig_t Config;/* local configure parameters */
#ifdef MCS_MODULE_PLC
	u_int32_t SetPLCMCEvent;	/* Event bit map */
	struct evloopTimeout SetPLCMCTimer;	/* evloop timer */
	MCS_BOOL PLCFirmwareDown;
#endif
	struct hmap snooperMap;		/*Snooper container*/
} mcManagerS;

#define mcManagerDebug(level, ...) \
                 dbgf(mcManagerS.DebugModule,(level),__VA_ARGS__)
#define mcManagerTRACE() mcManagerDebug(DBGDUMP, "ENTER %s", __func__)

static void mcManagerMCReadBufCB(void *Cookie /*unused*/ );

/*========================================================================*/
/*============ Internal handling =========================================*/
/*========================================================================*/
static int mcManagerCreateNLSock(int32_t netlinkKey, int32_t groups)
{
	int32_t NLSock;
	struct sockaddr_nl Local;

	if ((NLSock = socket(AF_NETLINK, SOCK_RAW, netlinkKey)) < 0) {
		mcManagerDebug(DBGERR, "%s: Create netlink socket failed", __func__);
		goto out;
	}

	/* Set nonblock. */
	if (fcntl(NLSock, F_SETFL, fcntl(NLSock, F_GETFL) | O_NONBLOCK)) {
		mcManagerDebug(DBGERR, "%s fcntl() failed", __func__);
		goto err;
	}

	memset(&Local, 0, sizeof Local);
	Local.nl_family = AF_NETLINK;
	Local.nl_pid = getpid();	/* self pid */
	Local.nl_groups = groups;

	if (bind(NLSock, (struct sockaddr *)&Local, sizeof Local) < 0) {
		mcManagerDebug(DBGERR, "%s: Bind netlink socket failed", __func__);
		goto err;
	}

	return NLSock;

err:
	close(NLSock);
out:
	return -1;
}

static void mcManagerMCReadbufRegister(void)
{
	u_int32_t RdBufSize = NLMSG_SPACE(mcManagerReadBufLen);

	mcManagerS.MCNLSock = mcManagerCreateNLSock(NETLINK_QCA_MC, 0);

	__ASSERT_FATAL(mcManagerS.MCNLSock >= 0,
		"Failed to create Netlink socket for multicast snooping");

	/* Initialize input context */
	bufrdCreate(&mcManagerS.MCReadBuf, "mcManager-rd-mc", mcManagerS.MCNLSock, RdBufSize,	/* Read buf size */
		mcManagerMCReadBufCB,	/* callback */
		NULL);
}

static void mcManagerMCReadbufUnRegister(void)
{
	close(mcManagerS.MCNLSock);
	bufrdDestroy(&mcManagerS.MCReadBuf);
}

static void mcManagerInitForwardTablePlugins(interface_t *iface)
{

	/*
	 * Each type init will be init really once, once it done, call will be
	 * return at once.
	 */
	switch (iface->type) {
		/* Plugins */
#ifdef MCS_MODULE_WLAN
	case interfaceType_WLAN2G:
		WLAN2G_InitForwardTablePlugin(iface);
		break;
	case interfaceType_WLAN5G:
		WLAN5G_InitForwardTablePlugin(iface);
		break;
#endif
	case interfaceType_ESWITCH:
		ESWITCH_InitForwardTablePlugin(iface);
		break;
	default:
		break;
	}
}

static int mcManagerFlushForwardTable(interface_t *iface)
{
	if (!iface)
		return -1;

	switch (iface->type) {
		/* Plugins */
#ifdef MCS_MODULE_WLAN
	case interfaceType_WLAN2G:
		return WLAN2G_FlushForwardTable(iface);
	case interfaceType_WLAN5G:
		return WLAN5G_FlushForwardTable(iface);
#endif
	case interfaceType_ESWITCH:
		return ESWITCH_FlushForwardTable(iface);

#ifdef MCS_MODULE_PLC
		/* PLC */
	case interfaceType_PLC:
		return plcManagerFlushForwardTable(iface);
#endif

	default:
		return 0;
	}

	return -1;
}

static int mcManagerUpdateForwardTable(interface_t *iface, void *table, u_int32_t size)
{
	if (!iface)
		return -1;

	switch (iface->type) {
		/* Plugins */
#ifdef MCS_MODULE_WLAN
	case interfaceType_WLAN2G:
		return WLAN2G_UpdateForwardTable(iface, table, size);
	case interfaceType_WLAN5G:
		return WLAN5G_UpdateForwardTable(iface, table, size);
#endif
	case interfaceType_ESWITCH:
		return ESWITCH_UpdateForwardTable(iface, table, size);

#ifdef MCS_MODULE_PLC
		/* PLC */
	case interfaceType_PLC:
		return plcManagerUpdateForwardTable(iface, table, size);
#endif

	default:
		return 0;
	}

	return -1;
}

int mcManagerTableCmp(void *oldT, void *newT, int32_t oldCnt,
	int32_t newCnt, void *oldEntry, void *newEntry, int32_t entrySize)
{
	int32_t changed = 0;

	if (!oldT) {
		if (newT)
			changed = 1;
	} else if (!newT || oldCnt != newCnt ||
		(oldEntry && newEntry && memcmp(oldEntry, newEntry, oldCnt * entrySize))) {
		changed = 1;
	}

	return changed;
}

struct __mc_encaptbl_entry *mcManagerFindEncapEntryByGroup(struct __mc_group *group,
	struct mcManagerEncapTable *encapT)
{
	int32_t i = 0;
	struct __mc_encaptbl_entry *entry = NULL;

	if (!group) {
		mcManagerDebug(DBGERR, "%s: error, group is null\n", __func__);
		return NULL;
	}

	if (!encapT || !encapT->entryCnt)
		return NULL;

	entry = &(encapT->entry[0]);
	for (i = 0; i < encapT->entryCnt; i++) {
		if (!memcmp(group, &entry[i].group, sizeof(*group)))
			return &entry[i];
	}

	return NULL;
}

MCS_STATUS mcManagerAddEncapEntry(struct ether_addr *QCADevMAC, struct __mc_mdb_entry *mdbEntry,
	struct mcManagerEncapTable **encapT)
{
	struct __mc_encaptbl_entry *encapEntry = NULL;

	if (*encapT == NULL) {
		*encapT = malloc(sizeof(struct mcManagerEncapTable));
		if (*encapT == NULL) {
			mcManagerDebug(DBGERR, "%s: error, no memory for EncapTable\n", __func__);
			return MCS_NOK;
		}
		memset(*encapT, 0, sizeof(struct mcManagerEncapTable));
	} else if ((*encapT)->entryCnt >= MC_DEF_GROUP_MAX) {
		mcManagerDebug(DBGINFO, "%s: encap table is full!!", __func__);
		return MCS_OK;
	}

	encapEntry = &((*encapT)->entry[(*encapT)->entryCnt]);
	encapEntry->dev_cnt = 1;
	memcpy(&encapEntry->group, &mdbEntry->group, sizeof(struct __mc_group));
	memcpy(encapEntry->dev[0].mac, QCADevMAC, ETH_ALEN);
	if (mdbEntry->filter_mode == MC_DEF_FILTER_INCLUDE) {
		/* Init ex_nsrcs = MC_DEF_EX_SRCS_INVAL, mark as EXCLUDE list is invalid */
		encapEntry->dev[0].ex_nsrcs = MC_DEF_EX_SRCS_INVAL;
		encapEntry->dev[0].in_nsrcs = mdbEntry->nsrcs;
		memcpy(encapEntry->dev[0].in_srcs, mdbEntry->srcs,
			sizeof(encapEntry->dev[0].in_srcs));
	} else if (mdbEntry->filter_mode == MC_DEF_FILTER_EXCLUDE) {
		encapEntry->dev[0].ex_nsrcs = mdbEntry->nsrcs;
		memcpy(encapEntry->dev[0].ex_srcs, mdbEntry->srcs,
			sizeof(encapEntry->dev[0].ex_srcs));
	}

	((*encapT)->entryCnt)++;

	return MCS_OK;
}

void mcManagerAddSourceList(u_int8_t *toSrcs, u_int32_t *toCnt, u_int8_t *fromSrcs,
	u_int32_t fromCnt, u_int32_t unitSize)
{
	int32_t i, j;

	if (!toSrcs || !fromSrcs || !unitSize || !fromCnt || !toCnt)
		return;

	if (*toCnt >= MC_DEF_RT_SRCS_MAX)
		return;

	for (i = 0; i < fromCnt; i++) {
		for (j = 0; j < *toCnt; j++) {
			if (!memcmp(fromSrcs + i * unitSize, toSrcs + j * unitSize, unitSize))
				break;
		}

		if (j == *toCnt) {
			memcpy(toSrcs + (*toCnt) * unitSize, fromSrcs + i * unitSize, unitSize);
			(*toCnt)++;
			if (*toCnt >= MC_DEF_RT_SRCS_MAX) {
				mcManagerDebug(DBGERR, "Sources List buffer is full!");
				break;
			}
		}
	}
}

MCS_STATUS mcManagerUpdateEncapEntry(struct ether_addr *QCADevMAC,
	struct __mc_encaptbl_entry *encapEntry, struct __mc_mdb_entry *mdbEntry)
{
	int32_t i;
	struct __mc_encaptbl_dev *encapDev = NULL;

	if (!encapEntry || !mdbEntry)
		return MCS_OK;

	for (i = 0; i < encapEntry->dev_cnt; i++) {
		if (!memcmp(QCADevMAC, encapEntry->dev[i].mac, ETH_ALEN))
			break;
	}

	if (i == encapEntry->dev_cnt) {
		if (encapEntry->dev_cnt == MC_DEF_DEV_MAX) {
			mcManagerDebug(DBGINFO, "%s: encap device table is full!!", __func__);
			return MCS_OK;
		}
		encapDev = &(encapEntry->dev[encapEntry->dev_cnt]);
		memcpy(encapDev->mac, QCADevMAC, ETH_ALEN);
		encapEntry->dev_cnt++;
		if (mdbEntry->filter_mode == MC_DEF_FILTER_INCLUDE)
			encapDev->ex_nsrcs = MC_DEF_EX_SRCS_INVAL;
	} else {
		encapDev = &(encapEntry->dev[i]);
	}

	if (mdbEntry->filter_mode == MC_DEF_FILTER_INCLUDE) {
		mcManagerAddSourceList(encapDev->in_srcs, &encapDev->in_nsrcs,
			mdbEntry->srcs, mdbEntry->nsrcs,
			mdbEntry->group.pro ==
			htons(ETH_P_IP) ? sizeof(u_int32_t) : MC_DEF_IP6_SIZE);
	} else if (mdbEntry->filter_mode == MC_DEF_FILTER_EXCLUDE) {
		if (encapDev->ex_nsrcs == MC_DEF_EX_SRCS_INVAL)
			encapDev->ex_nsrcs = 0;
		mcManagerAddSourceList(encapDev->ex_srcs, &encapDev->ex_nsrcs,
			mdbEntry->srcs, mdbEntry->nsrcs,
			mdbEntry->group.pro ==
			htons(ETH_P_IP) ? sizeof(u_int32_t) : MC_DEF_IP6_SIZE);
	}

	return MCS_OK;
}

#ifdef  ENABLE_MC_ENCAP
MCS_STATUS mcManagerEncapTUpdate(struct ether_addr *QCADevMAC, struct __mc_mdb_entry *mdbEntry,
	struct mcManagerEncapTable **encapT)
{
	struct __mc_encaptbl_entry *encapEntry =
		mcManagerFindEncapEntryByGroup(&mdbEntry->group, *encapT);

	if (!encapEntry) {
		if (mcManagerAddEncapEntry(QCADevMAC, mdbEntry, encapT) == MCS_NOK) {
			mcManagerDebug(DBGERR, "%s: Add Encap Entry failed\n", __func__);
			return MCS_NOK;
		}
	} else {
		mcManagerUpdateEncapEntry(QCADevMAC, encapEntry, mdbEntry);
	}

	return MCS_OK;
}
#endif

struct __mc_floodtbl_entry *mcManagerFindFloodEntryByGroup(struct __mc_group *group,
	struct mcManagerFloodTable *floodT)
{
	int32_t i = 0;
	struct __mc_floodtbl_entry *entry = NULL;

	if (!group) {
		mcManagerDebug(DBGERR, "%s: error, group is null\n", __func__);
		return NULL;
	}

	if (!floodT || !floodT->entryCnt)
		return NULL;

	entry = floodT->entry;
	for (i = 0; i < floodT->entryCnt; i++) {
		if (!memcmp(group, &entry[i].group, sizeof(*group)))
			return &entry[i];
	}

	return NULL;
}

MCS_STATUS mcManagerAddFloodEntry(struct __mc_mdb_entry *mdbEntry,
	struct mcManagerFloodTable **floodT)
{
	struct __mc_floodtbl_entry *floodEntry = NULL;

	if (*floodT == NULL) {
		*floodT = malloc(sizeof(struct mcManagerFloodTable));
		if (*floodT == NULL) {
			mcManagerDebug(DBGERR, "%s: error, no memory for FloodTable\n", __func__);
			return MCS_NOK;
		}
		memset(*floodT, 0, sizeof(struct mcManagerFloodTable));
	}

	if ((*floodT)->entryCnt == MC_DEF_GROUP_MAX) {
		mcManagerDebug(DBGINFO, "%s: flood table is full", __func__);
		return MCS_OK;
	}

	floodEntry = &((*floodT)->entry[(*floodT)->entryCnt]);
	memcpy(&floodEntry->group, &mdbEntry->group, sizeof(struct __mc_group));
	floodEntry->ifcnt = 1;
	floodEntry->ifindex[0] = mdbEntry->ifindex;
	((*floodT)->entryCnt)++;

	return MCS_OK;
}

MCS_STATUS mcManagerUpdateFloodEntry(struct __mc_floodtbl_entry *floodEntry,
	struct __mc_mdb_entry *mdbEntry)
{
	int32_t i;

	if (!floodEntry || !mdbEntry)
		return MCS_OK;

	for (i = 0; i < floodEntry->ifcnt; i++) {
		if (mdbEntry->ifindex == floodEntry->ifindex[i])
			break;
	}

	if (i == floodEntry->ifcnt) {
		if (floodEntry->ifcnt == MC_DEF_IF_MAX) {
			mcManagerDebug(DBGINFO, "%s: flood interface table is full", __func__);
			return MCS_OK;
		}
		floodEntry->ifindex[floodEntry->ifcnt] = mdbEntry->ifindex;
		floodEntry->ifcnt++;
	}

	return MCS_OK;
}

MCS_STATUS mcManagerFloodTUpdate(struct __mc_mdb_entry *mdbEntry,
	struct mcManagerFloodTable **floodT)
{
	struct __mc_floodtbl_entry *floodEntry =
		mcManagerFindFloodEntryByGroup(&mdbEntry->group, *floodT);

	if (!floodEntry) {
		if (mcManagerAddFloodEntry(mdbEntry, floodT) == MCS_NOK) {
			mcManagerDebug(DBGERR, "%s: Add Flood Entry failed\n", __func__);
			return MCS_NOK;
		}
	} else {
		mcManagerUpdateFloodEntry(floodEntry, mdbEntry);
	}

	return MCS_OK;
}

struct __mc_iftbl_entry *mcManagerFindIFEntryByGroup(struct __mc_group *group,
	struct mcManagerIFTable *ifT)
{
	int32_t i = 0;
	struct __mc_iftbl_entry *entry = NULL;

	if (!group) {
		mcManagerDebug(DBGERR, "%s: error, group is null\n", __func__);
		return NULL;
	}

	if (!ifT || !ifT->entryCnt)
		return NULL;

	entry = &(ifT->entry[0]);
	for (i = 0; i < ifT->entryCnt; i++) {
		if (!memcmp(group, &entry[i].group, sizeof(*group)))
			return &entry[i];
	}
	return NULL;
}

MCS_STATUS mcManagerAddIFEntry(struct __mc_mdb_entry *mdbEntry, struct mcManagerIFTable *ifT)
{
	struct __mc_iftbl_entry *ifEntry = NULL;

	if ((ifT)->entryCnt == MC_DEF_GROUP_MAX) {
		mcManagerDebug(DBGINFO, "%s: if table is full!!", __func__);
		return MCS_OK;
	}

	ifEntry = &(ifT->entry[ifT->entryCnt]);
	ifEntry->node_cnt = 1;
	memcpy(&ifEntry->group, &mdbEntry->group, sizeof(struct __mc_group));
	memcpy(ifEntry->nodes[0].mac, mdbEntry->mac, ETH_ALEN);

	if (mdbEntry->filter_mode == MC_DEF_FILTER_INCLUDE
		|| mdbEntry->filter_mode == MC_DEF_FILTER_EXCLUDE) {
		ifEntry->nodes[0].filter_mode = mdbEntry->filter_mode;
		ifEntry->nodes[0].nsrcs = mdbEntry->nsrcs;
		memcpy(ifEntry->nodes[0].srcs, mdbEntry->srcs, sizeof(ifEntry->nodes[0].srcs));
	}

	ifT->entryCnt++;

	return MCS_OK;
}

MCS_STATUS mcManagerUpdateIFEntry(struct __mc_iftbl_entry *ifEntry,
	struct __mc_mdb_entry *mdbEntry, struct mcManagerIFTable *ifT)
{
	int32_t i;
	struct __mc_iftbl_node *node = NULL;

	if (!ifT || !ifEntry || !mdbEntry)
		return MCS_NOK;

	for (i = 0; i < ifEntry->node_cnt; i++) {
		if (!memcmp(ifEntry->nodes[i].mac, mdbEntry->mac, ETH_ALEN))
			break;
	}

	if (i == ifEntry->node_cnt) {
		if (ifEntry->node_cnt == MC_DEF_IF_NODE_MAX) {
			mcManagerDebug(DBGINFO, "%s: if node table is full", __func__);
			return MCS_NOK;
		}

		node = &(ifEntry->nodes[ifEntry->node_cnt]);
		ifEntry->node_cnt++;
	} else {
		node = &(ifEntry->nodes[i]);
	}

	memcpy(node->mac, mdbEntry->mac, ETH_ALEN);
	node->filter_mode = mdbEntry->filter_mode;
	node->nsrcs = mdbEntry->nsrcs;
	memcpy(node->srcs, mdbEntry->srcs, sizeof(node->srcs));

	return MCS_OK;
}

MCS_STATUS mcManagerIFTUpdate(struct __mc_mdb_entry *mdbEntry, struct mcManagerIFNode *ifNode)
{
	/* Search and Update on the temptable */
	struct __mc_iftbl_entry *ifEntrytemp = mcManagerFindIFEntryByGroup(&mdbEntry->group, ifNode->temptable);

	if (!ifEntrytemp) {
		if (mcManagerAddIFEntry(mdbEntry, ifNode->temptable) == MCS_NOK) {
			mcManagerDebug(DBGERR, "%s: Add IF Entry failed\n", __func__);
			return MCS_NOK;
		}
		ifNode->sync = interfaceSync_NEW;
	} else {
		if (mcManagerUpdateIFEntry(ifEntrytemp, mdbEntry, ifNode->temptable) == MCS_NOK) {
			mcManagerDebug(DBGDEBUG, "%s: Not Update IF table\n", __func__);
		}
		ifNode->sync = interfaceSync_UPDATED;
	}

	return MCS_OK;
}

MCS_STATUS mcManagerEncapTableCLR(const char *BridgeName, bridgeTable_e type, void **table)
{
	int32_t numEntries = 0;
	u_int8_t *entry;

	if (!BridgeName || (type != MC_BRIDGE_ENCAP_TABLE && type != MC_BRIDGE_FLOOD_TABLE))
		return MCS_NOK;

	if ((entry = bridgeAllocTableBuf(0, BridgeName)) == NULL)
		return MCS_NOK;

	if (bridgeSetTableAsyn(BridgeName, type, &numEntries, entry) < 0) {
		bridgeFreeTableBuf(entry);
		mcManagerDebug(DBGERR, "%s: Clear multicast table %d failed!", __func__, type);
		return MCS_NOK;
	}
	bridgeFreeTableBuf(entry);

	if (*table)
		free(*table);
	*table = NULL;

	return MCS_OK;
}

void mcManagerIFTableCLR(struct mcManagerIFNode **ifT, interface_t *interface)
{
	if (!*ifT || !interface)
		return;

	mcManagerFlushForwardTable(interface);

#if HAN_MCS
    if ((*ifT)->table) {
        free((*ifT)->table);
        (*ifT)->table = NULL;
    }
    if ((*ifT)->temptable) {
        free((*ifT)->temptable);
        (*ifT)->temptable = NULL;
    }
#endif
	free(*ifT);
	*ifT = NULL;
}

void mcManagerGetMDB(const char *brName)
{
	struct __mc_mdb_entry *mdbEntry;
	int32_t num_entries;
//	interface_t *Bridge = interface_getBridge();

	num_entries = mcManagerNumMDBEntry;
	if ((mdbEntry = bridgeAllocTableBuf(num_entries * sizeof(struct __mc_mdb_entry),
				brName)) == NULL) {
		mcManagerDebug(DBGERR, "%s: can't alloc buffer of num_entries %d\n", __func__,
			num_entries);
		return;
	}

	if (bridgeGetTableAsyn(brName, MC_BRIDGE_MDB_TABLE, &num_entries, mdbEntry) < 0) {
		mcManagerDebug(DBGERR, "%s: failed to execute mdb command, num_entries %d\n",
			__func__, num_entries);
	}

	bridgeFreeTableBuf(mdbEntry);

	return;
}

void mcManagerFlushSnooper(struct mcManagerSnooper *snooper)
{
	struct hmap_node *node, *n;
	struct mcManagerIFNode *ifTemp = NULL;

	mcManagerDebug(DBGDEBUG, "%s:Flush snooper[%s]", __func__, snooper->node.name);
	mcManagerEncapTableCLR(snooper->node.name, MC_BRIDGE_ENCAP_TABLE,(void **)&snooper->encapT);
	mcManagerEncapTableCLR(snooper->node.name, MC_BRIDGE_FLOOD_TABLE,(void **)&snooper->floodT);

	HMapForEachSafe(node, n, &(snooper->ifTableMap)) {
		hmapRemove(&snooper->ifTableMap, node);
		ifTemp = HMapEntry(node, struct mcManagerIFNode, node);
		mcManagerIFTableCLR(&ifTemp, &ifTemp->interface);
	}
}

void mcManagerMCProcess(struct mcManagerSnooper *snooper, struct __mc_mdb_entry *mdbEntry, int32_t num_entries)
{
	int32_t i;
#ifdef  ENABLE_MC_ENCAP
	/*encap related should be remove completely, this is no such support*/
	struct mcManagerEncapTable *encapT = NULL;
#endif
	struct mcManagerFloodTable *floodT = NULL;
	struct mcManagerIFNode *ifTemp = NULL;
	struct hmap_node *node, *n;
	struct mcManagerIFTable *tmptbl = NULL;
	bool modified = false;

	char ifName[IFNAMSIZ];
#if HAN_MCS
	bool flag = false;
	char real_ifName[IFNAMSIZ] = {0};
#endif

	if (num_entries == 0) {
		mcManagerFlushSnooper(snooper);
		goto out;
	}
	for (i = 0; i < num_entries; i++, mdbEntry++) {
#ifdef  ENABLE_MC_ENCAP
		tdDeviceHandle_t handle =
			tdService_DBQCADeviceAttachedTo((struct ether_addr *)mdbEntry->mac);
		static const u_int8_t mac_zero[HD_ETH_ADDR_LEN] =
			{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		struct ether_addr *QCADevMAC = tdService_DBGetHybridDA(handle);
		struct __mc_encaptbl_entry *oldEncapEntry;

		if (QCADevMAC && (QCADevMAC->ether_addr_octet[0] & 0x01
				|| MACAddrEqual(QCADevMAC, mac_zero))) {
			mcManagerDebug(DBGERR,
				"%s: Fatal error, invalid QCADevMAC for " MACAddrFmt(":"), __func__,
				MACAddrData(mdbEntry->mac));
			continue;
		}
#endif
		mcManagerDebug(DBGDEBUG, "ifindex:%u", mdbEntry->ifindex);
		if (NULL == if_indextoname(mdbEntry->ifindex, ifName)) {
			mcManagerDebug(DBGDEBUG, "Can't Find ifName for	iif:%d",mdbEntry->ifindex);
			continue;
		}
		mcManagerDebug(DBGDEBUG, "device found:%s", ifName);
#if HAN_MCS
		/* ath001-untag, ath001-100 */
		if(strstr(ifName, "eth") != NULL) {
			continue;
		} else if (!strncmp(ifName, "ath", 3) && strlen(ifName) > 6) {
			strncpy(real_ifName, ifName, 6);
			flag = true;
			mcManagerDebug(DBGDEBUG, "convert device name to : %s", real_ifName);
			node = hmapLookUp(&snooper->ifTableMap, real_ifName);
			if (!node) {
				ifTemp = calloc(1, sizeof(struct mcManagerIFNode));

				if (ifTemp == NULL) {
					mcManagerDebug(DBGDEBUG, "Alloc failed");
					continue;
				}
				ifTemp->table = calloc(1, sizeof(struct mcManagerIFTable));
				if (ifTemp->table == NULL) {
					mcManagerDebug(DBGDEBUG, "Alloc failed for table");
					free(ifTemp);
					continue;
				}

				ifTemp->temptable = calloc(1, sizeof(struct mcManagerIFTable));
				if (ifTemp->temptable == NULL) {
					mcManagerDebug(DBGDEBUG, "Alloc failed (for table or temptable)");
					free(ifTemp->table);
					free(ifTemp);
					continue;
				}

				strlcpy(ifTemp->node.name, real_ifName, IFNAMSIZ);
				ifTemp->interface.systemIndex = if_nametoindex(real_ifName);
				ifTemp->interface.type = interface_getType(real_ifName);

				mcManagerDebug(DBGDEBUG, "Alloc New IF Table for %s type:%d", real_ifName, ifTemp->interface.type);
				hmapInsert(&snooper->ifTableMap, &ifTemp->node);
				ifTemp->sync = interfaceSync_NEW;
				mcManagerInitForwardTablePlugins(&ifTemp->interface);
			} else {
				ifTemp = HMapEntry(node, struct mcManagerIFNode, node);
			}
		} else {
			flag = true;
#endif
		node = hmapLookUp(&snooper->ifTableMap, ifName);
		if (!node) {
			ifTemp = calloc(1, sizeof(struct mcManagerIFNode));

			if (ifTemp == NULL) {
				mcManagerDebug(DBGDEBUG, "Alloc failed");
				continue;
			}

			ifTemp->table = calloc(1, sizeof(struct mcManagerIFTable));
			if (ifTemp->table == NULL) {
				mcManagerDebug(DBGDEBUG, "Alloc failed for table");
				free(ifTemp);
				continue;
			}

			ifTemp->temptable = calloc(1, sizeof(struct mcManagerIFTable));
			if (ifTemp->temptable == NULL) {
				mcManagerDebug(DBGDEBUG, "Alloc failed (for table or temptable)");
				free(ifTemp->table);
				free(ifTemp);
				continue;
			}

			strlcpy(ifTemp->node.name, ifName, IFNAMSIZ);
			ifTemp->interface.systemIndex = mdbEntry->ifindex;
			ifTemp->interface.type = interface_getType(ifName);

			mcManagerDebug(DBGDEBUG, "Alloc New IF Table for %s type:%d", ifName, ifTemp->interface.type);
			hmapInsert(&snooper->ifTableMap, &ifTemp->node);
			ifTemp->sync = interfaceSync_NEW;
			mcManagerInitForwardTablePlugins(&ifTemp->interface);
		} else {
			ifTemp = HMapEntry(node, struct mcManagerIFNode, node);
		}
#if HAN_MCS
    }
#endif
#ifdef  ENABLE_MC_ENCAP
		oldEncapEntry = mcManagerFindEncapEntryByGroup(&mdbEntry->group, snooper->encapT);
		if (QCADevMAC || (oldEncapEntry && mdbEntry->fdb_age_out)) {
			if (QCADevMAC) {
				mcManagerDebug(DBGDEBUG,
					"%s: QCA device node %d - " MACAddrFmt(":")
					", add it to encap table", __func__, i,
					MACAddrData(QCADevMAC->ether_addr_octet));
				if (mcManagerEncapTUpdate(QCADevMAC, mdbEntry, &encapT) == MCS_NOK)
					goto out;
			} else {
				int iDev;

				for (iDev = 0; iDev < oldEncapEntry->dev_cnt; iDev++) {
					if (mcManagerEncapTUpdate((struct ether_addr *)
							oldEncapEntry->dev[iDev].mac, mdbEntry,
							&encapT) == MCS_NOK)
						goto out;
				}
			}
		} else
#endif
		{
			mcManagerDebug(DBGDEBUG,
				"%s: Legacy node %d - " MACAddrFmt(":") ", add it to flood table",
				__func__, i, MACAddrData(mdbEntry->mac));
			if (mcManagerFloodTUpdate(mdbEntry, &floodT) == MCS_NOK)
				goto out;
			mcManagerDebug(DBGDEBUG, "%s: Legacy node %d, update forwarding table",
				__func__, i);
			if (mcManagerIFTUpdate(mdbEntry, ifTemp) == MCS_NOK)
				goto out;
		}
	}
#if HAN_MCS
    if(!flag)
        goto out;
#endif

#ifdef  ENABLE_MC_ENCAP
	if (mcManagerTableCmp(mcManagerS.encapT, encapT,
			mcManagerS.encapT ? mcManagerS.encapT->entryCnt : 0,
			encapT ? encapT->entryCnt : 0,
			mcManagerS.encapT ? mcManagerS.encapT->entry : NULL,
			encapT ? encapT->entry : NULL, sizeof(struct __mc_encaptbl_entry))) {
		mcManagerDebug(DBGDEBUG, "%s: Encap table is changed, update multicast encap table",
			__func__);
		if (!encapT->entryCnt) {
			mcManagerDebug(DBGDEBUG,
				"%s: Encap table is empty, flush multicast encap table", __func__);
			mcManagerEncapTableCLR(snooper->name, MC_BRIDGE_ENCAP_TABLE,
				(void **)&snooper->encapT);
		} else {
			struct __mc_encaptbl_entry *encapEntry = NULL;

			if ((encapEntry =
					bridgeAllocTableBuf(encapT->entryCnt *
						sizeof(struct __mc_encaptbl_entry),
						snooper->name)) == NULL)
				goto out;

			mcManagerDebug(DBGDEBUG,
				"%s: Encap table is non-empty, update multicast encap table, entry count is %d, table size is %d",
				__func__, encapT->entryCnt,
				encapT->entryCnt * sizeof(struct __mc_encaptbl_entry));
			num_entries = encapT->entryCnt;
			memcpy(encapEntry, encapT->entry,
				num_entries * sizeof(struct __mc_encaptbl_entry));
			if (bridgeSetTableAsyn(snooper->name, MC_BRIDGE_ENCAP_TABLE, &num_entries,
					encapEntry) < 0) {
				mcManagerDebug(DBGERR, "%s: Update multicast encap table failed",
					__func__);
				bridgeFreeTableBuf(encapEntry);
				goto out;
			}
			bridgeFreeTableBuf(encapEntry);
			if (snooper->encapT)
				free(snooper->encapT);
			snooper->encapT = encapT;
			encapT = NULL;
		}
	}
#endif
	if (mcManagerTableCmp(snooper->floodT, floodT,
			snooper->floodT ? snooper->floodT->entryCnt : 0,
			floodT ? floodT->entryCnt : 0,
			snooper->floodT ? snooper->floodT->entry : NULL,
			floodT ? floodT->entry : NULL, sizeof(struct __mc_floodtbl_entry))) {
		mcManagerDebug(DBGDEBUG, "%s: Flood table is changed, update multicast flood table",
			__func__);
		if (!floodT || !floodT->entryCnt) {
			mcManagerDebug(DBGDEBUG,
				"%s: Flood table is empty, flush multicast flood table", __func__);
			mcManagerEncapTableCLR(snooper->node.name, MC_BRIDGE_FLOOD_TABLE,
				(void **)&snooper->floodT);
		} else {
			struct __mc_floodtbl_entry *floodEntry = NULL;

			if ((floodEntry =
					bridgeAllocTableBuf(floodT->entryCnt *
						sizeof(struct __mc_floodtbl_entry),
						snooper->node.name)) == NULL)
				goto out;

			mcManagerDebug(DBGDEBUG,
				"%s: Flood table is non-empty, update multicast flood table, entry count is %d",
				__func__, floodT->entryCnt);
			num_entries = floodT->entryCnt;
			memcpy(floodEntry, floodT->entry,
				num_entries * sizeof(struct __mc_floodtbl_entry));
			if (bridgeSetTableAsyn(snooper->node.name, MC_BRIDGE_FLOOD_TABLE, &num_entries,
					floodEntry) < 0) {
				mcManagerDebug(DBGERR, "%s: Update multicast flood table failed",
					__func__);
				bridgeFreeTableBuf(floodEntry);
				goto out;
			}
			bridgeFreeTableBuf(floodEntry);
			if (snooper->floodT)
				free(snooper->floodT);
			snooper->floodT = floodT;
			floodT = NULL;
		}
	}

	HMapForEachSafe(node, n, &snooper->ifTableMap) {
		ifTemp = HMapEntry(node, struct mcManagerIFNode, node);
		if (ifTemp->sync) {
			modified = (ifTemp->table->entryCnt == ifTemp->temptable->entryCnt)? false:true;
			if (!modified) {
				modified = memcmp(ifTemp->table->entry, ifTemp->temptable->entry, sizeof(struct __mc_iftbl_entry) * ifTemp->table->entryCnt);
			}
			if (modified) {
				if (mcManagerUpdateForwardTable(&ifTemp->interface, ifTemp->temptable, sizeof(struct mcManagerIFTable)) < 0) {
					mcManagerDebug(DBGERR, "%s: Failed to update interface %s forwarding table",__func__, ifTemp->node.name);
				} else {
					mcManagerDebug(DBGDEBUG,"%s: Updated interface %s forwarding table successfully",__func__, ifTemp->node.name);
				}
			}
			ifTemp->sync = interfaceSync_DONE;

			/* Swap table and temptable pointers
			 * so that ifTemp->table is pointing to the updated table.
			 * But also zeroes out the temptable->entryCnt so that the next time the deletion of a client can be reflected (update is on top of ifTemp->temptable).
			*/
			tmptbl = ifTemp->table;
			ifTemp->table = ifTemp->temptable;
			ifTemp->temptable = tmptbl;
			ifTemp->temptable->entryCnt = 0;
		} else {
			hmapRemove(&snooper->ifTableMap, node);
			mcManagerIFTableCLR(&ifTemp, &ifTemp->interface);
		}
	}

out:
#ifdef  ENABLE_MC_ENCAP
	if (encapT)
		free(encapT);
#endif
	if (floodT)
		free(floodT);
}

static void mcManagerMCReadBufCB(void *Cookie /*unused */ )
{
	struct bufrd *R = &mcManagerS.MCReadBuf;
	u_int32_t NMax = bufrdNBytesGet(R);
	u_int8_t *Buf = bufrdBufGet(R);
	struct nlmsghdr *NLh = (struct nlmsghdr *)Buf;
	struct __mcctl_msg_header *msgheader;
	struct __mc_mdb_entry *mdbEntry;
	int num_entries;
	int msghsize;

	mcManagerTRACE();

	/* Error check. */
	if (bufrdErrorGet(R)) {
		mcManagerDebug(DBGINFO, "%s: Read error!", __func__);

		mcManagerMCReadbufUnRegister();
		mcManagerMCReadbufRegister();
		return;
	}

	if (!NMax)
		return;

	msgheader = (struct __mcctl_msg_header *)NLMSG_DATA(NLh);
	mcManagerDebug(DBGDEBUG, "Get netlink MSG %d size %d\n", NLh->nlmsg_type, NMax);
	switch (NLh->nlmsg_type) {
	case MC_EVENT_MDB_UPDATED:
		mcManagerDebug(DBGDEBUG, "%s: Receive mdb updated event from device[%s]\n",
			       __func__, msgheader->if_name);
		mcManagerGetMDB(msgheader->if_name);
		break;

	case MC_MSG_GET_MDB:
		if (msgheader->status == MC_STATUS_BUFFER_OVERFLOW) {
			num_entries = msgheader->bytes_needed / sizeof(struct __mc_mdb_entry);
			mcManagerDebug(DBGINFO, "No enough buffer for mdb entries %d\n",
				num_entries);
			if (num_entries <= mcManagerNumMDBEntry) {
				mcManagerDebug(DBGERR, "Invalid mdb entries %d/%d\n", num_entries,
					mcManagerNumMDBEntry);
				break;
			}
			mcManagerMCReadbufUnRegister();
			mcManagerReadBufLen +=
				sizeof(struct __mc_mdb_entry) * (num_entries -
				mcManagerNumMDBEntry);
			mcManagerNumMDBEntry = num_entries;
			mcManagerMCReadbufRegister();
			mcManagerGetMDB(msgheader->if_name);
			return;
		} else if (msgheader->status == MC_STATUS_SUCCESS) {
			struct mcManagerSnooper *snooper;
			struct hmap_node * node;
			node =  hmapLookUp(&mcManagerS.snooperMap, msgheader->if_name);
			if (!node) {
				mcManagerDebug(DBGERR,"Can't find snooper for %s",
						msgheader->if_name);
				break;
			}
			snooper = HMapEntry(node, struct mcManagerSnooper, node);
			msghsize = MC_BRIDGE_MESSAGE_SIZE(0);
			if (NMax < msghsize + msgheader->bytes_written) {
				mcManagerDebug(DBGERR,
					"Invalid MDB message size max %d header %d data %d\n", NMax,
					msghsize, msgheader->bytes_written);
				break;
			}

			num_entries = msgheader->bytes_written / sizeof(struct __mc_mdb_entry);
			mcManagerDebug(DBGINFO, "num_entries: %d", num_entries);
			mdbEntry = (struct __mc_mdb_entry *)(Buf + msghsize);
			mcManagerMCProcess(snooper, mdbEntry, num_entries);
		} else {
			mcManagerDebug(DBGERR, "Failed to get mdb entries\n");
			break;
		}
		break;

	case MC_MSG_SET_PSW_ENCAP:
	case MC_MSG_SET_PSW_FLOOD:
		if (msgheader->status != MC_STATUS_SUCCESS) {
			mcManagerDebug(DBGERR, "Failed to execute netlink command %d\n",
				NLh->nlmsg_type);
		}
		break;

	default:
		mcManagerDebug(DBGINFO, "%s: Unknown netlink message type %d",
			__func__, NLh->nlmsg_type);
	}

	bufrdConsume(R, NMax);
}

/*===========================================================================*/
/*================= Optional Debug Menu======================================*/
/*===========================================================================*/
#if 1				//def MCS_DBG_MENU /* entire debug menu section */
#include <cmd.h>

/* ------------------- encap table -------------------------------- */
#define MC_IP4_FMT(ip4)     (ip4)[0], (ip4)[1], (ip4)[2], (ip4)[3]
#define MC_IP4_STR          "%03d.%03d.%03d.%03d"
#define MC_IP6_FMT(ip6)     ntohs((ip6)[0]), ntohs((ip6)[1]), ntohs((ip6)[2]), ntohs((ip6)[3]), ntohs((ip6)[4]), ntohs((ip6)[5]), ntohs((ip6)[6]), ntohs((ip6)[7])
#define MC_IP6_STR          "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x"
#define MC_MAC_FMT(addr)    (addr)[0], (addr)[1], (addr)[2], (addr)[3], (addr)[4], (addr)[5]
#define MC_MAC_STR          "%02x:%02x:%02x:%02x:%02x:%02x"

static const char *mcManagerMenuEncapHelp[] = {
	"encap -- print encapsulation table",
	NULL
};

static void mcManagerMenuEncapDump(struct cmdContext *Context, u_int16_t pro,
		u_int32_t *index, struct mcManagerEncapTable *encapT)
{
	u_int32_t i, j, k;
	u_int16_t *pIp;
	char group_string[64];
	struct __mc_encaptbl_entry *entry;

	for (i = 0; i < encapT->entryCnt; i++) {
		entry = &encapT->entry[i];
		pIp = (u_int16_t *)&entry->group.u.ip6;

		if (entry->group.pro != htons(pro))
			continue;

		if (pro == ETH_P_IP) {
			snprintf(group_string, sizeof group_string, MC_IP4_STR,
				MC_IP4_FMT((unsigned char *)&entry->group.u.ip4));
		} else {
			snprintf(group_string, sizeof group_string, MC_IP6_STR,
				MC_IP6_FMT(pIp));
		}
		cmdf(Context, "\t%-6d%-42s", (*index)++, group_string);

		if (!entry->dev_cnt)
			cmdf(Context, "--Num of QCA device:%d\n", entry->dev_cnt);
		else
			cmdf(Context, "--Num of QCA device:%d\n", entry->dev_cnt);
		for (j = 0; j < entry->dev_cnt; j++) {
			if (j == entry->dev_cnt - 1)
				cmdf(Context,
					"\t                                                --QCA device MAC %d of %d:"
					MC_MAC_STR "\n", j + 1, entry->dev_cnt,
					MC_MAC_FMT(entry->dev[j].mac));
			else
				cmdf(Context,
					"\t                                                --QCA device MAC %d of %d:"
					MC_MAC_STR "\n", j + 1, entry->dev_cnt,
					MC_MAC_FMT(entry->dev[j].mac));

			cmdf(Context,
				"\t                                                    --Num of Non blocked sources:%d\n",
				entry->dev[j].in_nsrcs);
			for (k = 0; k < entry->dev[j].in_nsrcs; k++) {
				if (pro == ETH_P_IP) {
					unsigned int *source =
						(unsigned int *)entry->dev[j].in_srcs;
					snprintf(group_string, sizeof group_string, MC_IP4_STR,
						MC_IP4_FMT((unsigned char *)(&source[k])));
				} else {
					struct in6_addr *source =
						(struct in6_addr *)entry->dev[j].in_srcs;
					snprintf(group_string, sizeof group_string, MC_IP6_STR,
						MC_IP6_FMT((unsigned short *)(&source[k])));
				}
				if (k == entry->dev[j].in_nsrcs - 1)
					cmdf(Context,
						"\t                                                     `--Non blocked sources %d of %d:%s\n",
						k + 1, entry->dev[j].in_nsrcs, group_string);
				else
					cmdf(Context,
						"\t                                                     |--Non blocked sources %d of %d:%s\n",
						k + 1, entry->dev[j].in_nsrcs, group_string);

			}
			if (!entry->dev[j].ex_nsrcs
				|| entry->dev[j].ex_nsrcs == MC_DEF_EX_SRCS_INVAL)
				cmdf(Context,
					"\t                                                    --Num of blocked sources:0\n");
			else
				cmdf(Context,
					"\t                                                    --Num of blocked sources:%d\n",
					entry->dev[j].ex_nsrcs);
			for (k = 0;
				entry->dev[j].ex_nsrcs <= MC_DEF_RT_SRCS_MAX
				&& k < entry->dev[j].ex_nsrcs; k++) {
				if (pro == ETH_P_IP) {
					unsigned int *source =
						(unsigned int *)entry->dev[j].ex_srcs;
					snprintf(group_string, sizeof group_string, MC_IP4_STR,
						MC_IP4_FMT((unsigned char *)(&source[k])));
				} else {
					struct in6_addr *source =
						(struct in6_addr *)entry->dev[j].ex_srcs;
					snprintf(group_string, sizeof group_string, MC_IP6_STR,
						MC_IP6_FMT((unsigned short *)(&source[k])));
				}
				if (k == entry->dev[j].ex_nsrcs - 1)
					cmdf(Context,
						"\t                                                     `--Blocked sources %d of %d:%s\n",
						k + 1, entry->dev[j].ex_nsrcs, group_string);
				else
					cmdf(Context,
						"\t                                                     |--Blocked sources %d of %d:%s\n",
						k + 1, entry->dev[j].ex_nsrcs, group_string);
			}
		}
	}
}

static void mcManagerMenuEncapHandler(struct cmdContext *Context, const char *Cmd)
{
	u_int32_t index = 0;
	struct mcManagerEncapTable *encapT;
	struct mcManagerSnooper *snooper;

	struct hmap_node *node;
	HMapForEach(node, &(mcManagerS.snooperMap)) {
		snooper = HMapEntry(node, struct mcManagerSnooper, node);
		cmdf(Context,
		"\n\n---------------------------Multicast Encapsulation Table[%s]--------------------\n", node->name);
		encapT = snooper->encapT;
		if (!encapT || !encapT->entryCnt) {
			cmdf(Context, "Encap table is empty!\n\n");
			continue;
		}
		cmdf(Context, "Encap table contains %d entries\n", encapT->entryCnt);
		cmdf(Context, "\tIndex IGMP-Group                                QCA-DEV-LIST\n");
		mcManagerMenuEncapDump(Context, ETH_P_IP, &index, encapT);
		cmdf(Context, "\n\n");

	}
}

/* ------------------- flood table -------------------------------- */
static const char *mcManagerMenuFloodHelp[] = {
	"flood -- print flood table",
	NULL
};

static void mcManagerMenuFloodDump(struct cmdContext *Context, u_int16_t pro,
		u_int32_t *index, struct mcManagerFloodTable *floodT)
{
	u_int32_t i, j;
	u_int16_t *pIp;
	char group_string[64];
	struct __mc_floodtbl_entry *entry;

	for (i = 0; i < floodT->entryCnt; i++) {
		entry = &floodT->entry[i];
		pIp = (u_int16_t *)&entry->group.u.ip6;

		if (entry->group.pro != htons(pro))
			continue;

		if (pro == ETH_P_IP)
			snprintf(group_string, sizeof group_string, MC_IP4_STR,
				MC_IP4_FMT((unsigned char *)&entry->group.u.ip4));
		else
			snprintf(group_string, sizeof group_string, MC_IP6_STR,
				MC_IP6_FMT(pIp));
		cmdf(Context, "\t%-6d%-42s", (*index)++, group_string);

		for (j = 0; j < entry->ifcnt; j++) {
			char ifName[IFNAMSIZ];
			if_indextoname(entry->ifindex[j], ifName);
			if (!j)
				cmdf(Context, "%s\n", ifName);
			else
				cmdf(Context,
					"\t                                                %s\n", ifName);
		}
	}
}

static void mcManagerMenuFloodHandler(struct cmdContext *Context, const char *Cmd)
{
	u_int32_t index = 0;
	struct mcManagerFloodTable *floodT;
	struct mcManagerSnooper *snooper;

	struct hmap_node *node;
	HMapForEach(node, &(mcManagerS.snooperMap)) {
		snooper = HMapEntry(node, struct mcManagerSnooper, node);
		cmdf(Context,
		"\n\n---------------------------Multicast Flood Table[%s]--------------------\n", node->name);
		floodT = snooper->floodT;
		if (!floodT || !floodT->entryCnt) {
			cmdf(Context, "Flood table is empty!\n\n");
			continue;
		}
		cmdf(Context, "Flood table contains %d entries\n", floodT->entryCnt);
		cmdf(Context, "\tIndex IGMP-Group                                Flood-Interface-List\n");
		mcManagerMenuFloodDump(Context, ETH_P_IP, &index, floodT);
		cmdf(Context, "\n\tIndex MLD-Group                                 Flood-Interface-List\n");
		mcManagerMenuFloodDump(Context, ETH_P_IPV6, &index, floodT);
		cmdf(Context, "\n\n");

	}
}

/* ------------------- Interface forwarding table -------------------------------- */
static const char *mcManagerMenuIffwdHelp[] = {
	"iffwd-- print interface forwarding table",
	NULL
};

static void mcManagerMenuIffwdDump(struct cmdContext *Context,
	struct mcManagerIFTable *ifT, u_int16_t pro, u_int32_t *index)
{
	u_int32_t j, k, l;
	u_int16_t *pIp;

	char group_string[64];
	struct __mc_iftbl_entry *entry;

	for (j = 0; j < ifT->entryCnt; j++) {
		entry = &ifT->entry[j];
		pIp = (u_int16_t *)&entry->group.u.ip6;

		if (entry->group.pro != htons(pro))
			continue;

		if (pro == ETH_P_IP)
			snprintf(group_string, sizeof group_string, MC_IP4_STR,
				MC_IP4_FMT((unsigned char *)&entry->group.u.ip4));
		else
			snprintf(group_string, sizeof group_string, MC_IP6_STR,
				MC_IP6_FMT(pIp));
		cmdf(Context, "\t%-6d%-42s", (*index)++, group_string);
		if (!entry->node_cnt)
			cmdf(Context, "\n");

		for (k = 0; k < entry->node_cnt; k++) {
			if (!k)
				cmdf(Context, "Node %d of %d:" MC_MAC_STR "\n",
					k + 1, entry->node_cnt, MC_MAC_FMT(entry->nodes[k].mac));
			else
				cmdf(Context,
					"\t                                                Node %d of %d:"
					MC_MAC_STR "\n", k + 1, entry->node_cnt,
					MC_MAC_FMT(entry->nodes[k].mac));

			if (entry->nodes[k].filter_mode) {
				cmdf(Context,
					"\t                                                 --Source Mode:%s\n",
					entry->nodes[k].filter_mode ==
					1 ? "Nonblock Listed Sources" : "Block Listed Sources");
				if (entry->nodes[k].nsrcs)
					cmdf(Context,
						"\t                                                 --Num of Sources:%d\n",
						entry->nodes[k].nsrcs);
				else
					cmdf(Context,
						"\t                                                 --Num of Sources:%d\n",
						entry->nodes[k].nsrcs);

				for (l = 0; l < entry->nodes[k].nsrcs; l++) {
					if (pro == ETH_P_IP) {
						unsigned int *source =
							(unsigned int *)entry->nodes[k].srcs;
						snprintf(group_string, sizeof group_string,
							MC_IP4_STR,
							MC_IP4_FMT((unsigned char *)(&source[l])));
					} else {
						struct in6_addr *source =
							(struct in6_addr *)entry->nodes[k].srcs;
						snprintf(group_string, sizeof group_string,
							MC_IP6_STR,
							MC_IP6_FMT((unsigned short *)(&source[l])));
					}
					if (l == entry->nodes[k].nsrcs - 1)
						cmdf(Context,
							"\t                                                  `--Source %d of %d:%s\n",
							l + 1, entry->nodes[k].nsrcs, group_string);
					else
						cmdf(Context,
							"\t                                                  |--Source %d of %d:%s\n",
							l + 1, entry->nodes[k].nsrcs, group_string);
				}
			}
		}
	}
}

static void mcManagerMenuIffwdHandler(struct cmdContext *Context, const char *Cmd)
{
	u_int32_t index = 0;
	struct hmap_node *node, *n;
	struct mcManagerSnooper *snooper;
	HMapForEach(node, &mcManagerS.snooperMap) {
		cmdf(Context,
			"\n\n------------------------Interface Forwarding Table[%s]-------------------------\n", node->name);
		snooper = HMapEntry(node, struct mcManagerSnooper, node);
		if(!snooper->ifTableMap.n) {
			cmdf(Context, "Table %-10s forwarding table is empty\n",
					node->name);
			continue;
		}
		HMapForEach(n, &(snooper->ifTableMap)) {
			struct mcManagerIFNode *ifTemp  = HMapEntry(n, struct mcManagerIFNode, node);
			uint32_t entryCnt = ifTemp->table->entryCnt;

			if (!entryCnt) {
				cmdf(Context, "Interface %-10s forwarding table is empty\n", n->name);
				continue;
			}

			cmdf(Context, "Interface %-10ss forwarding table contains %d entries\n",
				n->name);
			cmdf(Context,
				"\tIndex IGMP-Group                                Listen-Node-MAC-List\n");
			mcManagerMenuIffwdDump(Context, ifTemp->table, ETH_P_IP, &index);

			cmdf(Context,
				"\n\tIndex MLD-Group                                 Listen-Node-MAC-List\n");
			mcManagerMenuIffwdDump(Context, ifTemp->table, ETH_P_IPV6, &index);
			cmdf(Context, "\n");
		}
		cmdf(Context, "\n");
	}
}

static const struct cmdMenuItem mcManagerMenu[] = {
	CMD_MENU_STANDARD_STUFF(),
	{"encap", mcManagerMenuEncapHandler, NULL, mcManagerMenuEncapHelp},
	{"flood", mcManagerMenuFloodHandler, NULL, mcManagerMenuFloodHelp},
	{"iffwd", mcManagerMenuIffwdHandler, NULL, mcManagerMenuIffwdHelp},
	/* you can add more menu items here */
	CMD_MENU_END()
};

static const char *mcManagerMenuHelp[] = {
	"mc (mcManager) -- Multicast manager service",
	NULL
};

static const struct cmdMenuItem mcManagerMenuItem = {
	"mc",
	cmdMenu,
	(struct cmdMenuItem *)mcManagerMenu,
	mcManagerMenuHelp
};

#endif /* MCS_DBG_MENU  -- entire section */

/*--- mcManagerMenuInit -- add menu item for this module
*/
static void mcManagerMenuInit(void)
{
#ifdef MCS_DBG_MENU
	cmdMainMenuAdd(&mcManagerMenuItem);
#endif
}

#ifdef MCS_MODULE_PLC
void mcManagerSetPLCMCTimerHandler(void *Cookie)
{
	if (mcManagerS.SetPLCMCEvent & mcManagerEventPLCSnoopingFail) {
		/* If bridge snooping feature is enabled, the PLC snooping feature will always be disabled. */
		plcManagerSetSnoopingDisable();
	} else if (mcManagerS.SetPLCMCEvent & mcManagerEventPLCTableFail) {
		/* Re-Update Plc multicast forwarding table */
		interface_t *PlcIface = interface_getInterfaceFromType(interfaceType_PLC);

		if (PlcIface && PlcIface->index < INTERFACE_MAX_INTERFACES)
			mcManagerUpdateForwardTable(PlcIface, mcManagerS.ifT[PlcIface->index],
				sizeof(struct mcManagerIFTable));
	}

	mcManagerS.SetPLCMCEvent = 0;
}

void mcManagerEventSetMCFail(struct mdEventNode *Event)
{
	if (!MCS_ASSERT(Event && Event->Data, "Event CB should never have a NULL Event argument")) {
		return;
	}

	switch (*((u_int8_t *) Event->Data)) {
	case SM_EVENT_PLC_DOWN:
		mcManagerS.PLCFirmwareDown = MCS_TRUE;
		evloopTimeoutUnregister(&mcManagerS.SetPLCMCTimer);
		mcManagerDebug(DBGDEBUG,
			"%s: PLC firmware DOWN event, cancel snooping re-set timer", __func__);
		goto out;
	case SM_EVENT_PLC_UP:
		if (mcManagerS.PLCFirmwareDown == MCS_FALSE) {
			mcManagerDebug(DBGDUMP,
				"%s: PLC firmware UP event, ignore as it's already UP", __func__);
			goto out;
		}

		mcManagerS.PLCFirmwareDown = MCS_FALSE;
		mcManagerS.SetPLCMCEvent |= mcManagerEventPLCSnoopingFail;
		mcManagerS.SetPLCMCEvent |= mcManagerEventPLCTableFail;
		mcManagerDebug(DBGDEBUG,
			"%s: PLC firmware UP event, disable snooping and update forwarding table",
			__func__);
		break;
	case SM_SET_IGMP_SNOOPING_FAILURE:
		if (mcManagerS.PLCFirmwareDown == MCS_TRUE) {
			mcManagerDebug(DBGDUMP,
				"%s: PLC firmware is DOWN, ignore disabling snooping failure event",
				__func__);
			goto out;
		}
		mcManagerS.SetPLCMCEvent |= mcManagerEventPLCSnoopingFail;
		mcManagerDebug(DBGINFO, "%s: Set PLC snooping to (enable/disable) failed",
			__func__);
		break;
	case SM_UPDATE_HIFI_TABLE_FAILURE:
		if (mcManagerS.PLCFirmwareDown == MCS_TRUE) {
			mcManagerDebug(DBGDUMP,
				"%s: PLC firmware is DOWN, ignore setting forwarding table failure event",
				__func__);
			goto out;
		}
		mcManagerS.SetPLCMCEvent |= mcManagerEventPLCTableFail;
		mcManagerDebug(DBGINFO, "%s: Update PLC multicast forwarding table failed",
			__func__);
		break;
	default:
		mcManagerDebug(DBGINFO, "%s: Invalid event %d", __func__,
			*((u_int8_t *) Event->Data));
		goto out;
	}
	/* register evloop timeout */
	evloopTimeoutRegister(&mcManagerS.SetPLCMCTimer, mcManagerSetPLCMCDelay, 0);
out:
	return;
}
#endif
/*========================================================================*/
/*============ Init ======================================================*/
/*========================================================================*/
void mcManagerListenInitCB(void)
{
#ifdef MCS_MODULE_PLC
	mdListenTableRegister(mdModuleID_Plc, plcManagerEvent_MCSnoopFail, mcManagerEventSetMCFail);
	mdListenTableRegister(mdModuleID_Plc, plcManagerEvent_MCTableFail, mcManagerEventSetMCFail);
	mdListenTableRegister(mdModuleID_Plc, plcManagerEvent_Link, mcManagerEventSetMCFail);
#endif
}
int mcManagerStart(const char *BrName)
{
	struct __mc_param_value Enable = { };
	struct mcManagerSnooper *snooper;
	struct hmap_node *node;
	int retval;
	mcManagerDebug(DBGDEBUG, "%s:Enter",__func__);
	node = hmapLookUp(&mcManagerS.snooperMap, BrName);
	if (node) {
		mcManagerDebug(DBGDEBUG, "snooper[%s] exists in the list", BrName);
		return 0;
	}
	snooper = calloc(1, sizeof(struct mcManagerSnooper));
	if (snooper == NULL) {
		mcManagerDebug(DBGDEBUG, "calloc failed");
		return -1;
	}
	strlcpy(snooper->node.name, BrName, IFNAMSIZ);
	Enable.val = 1;
	retval = bridgeSetSnoopingParam(BrName, MC_MSG_SET_ENABLE, &Enable,
			sizeof(Enable));
	mcManagerDebug(DBGDEBUG, "Enable snooper return:%d", retval);
	if (retval) {
		goto errorout;
	}
	hmapInit(&snooper->ifTableMap, 10);
	hmapInsert(&mcManagerS.snooperMap, &snooper->node);
	mcManagerGetMDB(BrName);
	mcManagerDebug(DBGDEBUG, "%s:Leave",__func__);
	return 0;

errorout:
	if (snooper)
		free(snooper);
	return 0;
}

int mcManagerStop(const char *BrName)
{
	struct hmap_node *node;
	struct mcManagerSnooper *snooper;
	mcManagerDebug(DBGDEBUG, "%s:Enter",__func__);
	node = hmapLookUp(&mcManagerS.snooperMap, BrName);
	if (node == NULL) {
		mcManagerDebug(DBGDEBUG, "%s:No snooper found for [%s]", __func__, BrName);
		return 0;
	}

	mcManagerDebug(DBGDEBUG, "%s:Stop snooper:%s",__func__, BrName);
	snooper = HMapEntry(node, struct mcManagerSnooper, node);
	hmapRemove(&mcManagerS.snooperMap, &snooper->node);
	mcManagerFlushSnooper(snooper);
	hmapFree(&snooper->ifTableMap);
	free(snooper);
	mcManagerDebug(DBGDEBUG, "%s:Leave",__func__);

	return 0;
}

int mcManagerStopAll(void)
{
	struct hmap_node *node, *n;
	struct mcManagerSnooper *snooper;
	int retval;
	struct __mc_param_value Enable = { };
	Enable.val = 0;
	HMapForEachSafe(node, n, &(mcManagerS.snooperMap)) {
		snooper = HMapEntry(node, struct mcManagerSnooper, node);
		hmapRemove(&mcManagerS.snooperMap, &snooper->node);
		mcManagerFlushSnooper(snooper);
		retval = bridgeSetSnoopingParam(node->name, MC_MSG_SET_ENABLE, &Enable, sizeof(Enable));
		mcManagerDebug(DBGDEBUG, "Disable snooper[%s] return:%d", node->name, retval);
		hmapFree(&snooper->ifTableMap);
		free(snooper);
	}

	return 0;
}

void mcManagerInit(void)
{
	if (mcManagerS.IsInit)
		return;

	memset(&mcManagerS, 0, sizeof mcManagerS);
	mcManagerS.IsInit = 1;

	mcManagerS.DebugModule = dbgModuleFind("mcManager");
	mcManagerDebug(DBGDEBUG, "ENTER mcManagerInit");
	mcManagerMenuInit();

	hmapInit(&mcManagerS.snooperMap, 10);
	bridgeSetEventInfo(NULL, getpid(), MC_MSG_SET_EVENT_PID, NETLINK_QCA_MC);
	mcManagerMCReadbufRegister();

#ifdef MCS_MODULE_PLC
	/* creat evloop timer */
	evloopTimeoutCreate(&mcManagerS.SetPLCMCTimer, "mcManagerSetPLCMCTimer", mcManagerSetPLCMCTimerHandler, NULL);	/* Cookie */
#endif

	//mcManagerInitForwardTablePlugins();


	mdListenInitCBRegister(mdModuleID_Mc, mcManagerListenInitCB);
}
