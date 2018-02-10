/** 
 *  Copyright (c) 1999~2017, Altibase Corp. and/or its affiliates. All rights reserved.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License, version 3,
 *  as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
 
/***********************************************************************
 * Copyright 1999-2001, ALTIBase Corporation or its subsidiaries.
 *
 * All rights reserved.
 **********************************************************************/

/***********************************************************************
 * $Id: smxTrans.cpp 82075 2018-01-17 06:39:52Z jina.kim $
 **********************************************************************/

#include <idl.h>
#include <idErrorCode.h>
#include <ideErrorMgr.h>
#include <smErrorCode.h>
#include <smr.h>
#include <smm.h>
#include <smp.h>
#include <svp.h>
#include <smc.h>
#include <sdc.h>
#include <smx.h>
#include <smxReq.h>
#include <sdr.h>
#include <sct.h>
#include <svrRecoveryMgr.h>
#include <sdpPageList.h>
#include <svcLob.h>
#include <smlLockMgr.h>

extern smLobModule sdcLobModule;

UInt           smxTrans::mAllocRSIdx = 0;
iduMemPool     smxTrans::mLobCursorPool;
iduMemPool     smxTrans::mLobColBufPool;
iduMemPool     smxTrans::mMutexListNodePool;
smrCompResPool smxTrans::mCompResPool;
smGetDtxGlobalTxId smxTrans::mGetDtxGlobalTxIdFunc;

static smxTrySetupViewSCNFuncs gSmxTrySetupViewSCNFuncs[SMI_STATEMENT_CURSOR_MASK+1]=
{
    NULL,
    smxTrans::trySetupMinMemViewSCN, // SMI_STATEMENT_MEMORY_CURSOR
    smxTrans::trySetupMinDskViewSCN, // SMI_STATEMENT_DISK_CURSOR
    smxTrans::trySetupMinAllViewSCN  // SMI_STATEMENT_ALL_CURSOR
};

IDE_RC smxTrans::initializeStatic()
{
    IDE_TEST( mCompResPool.initialize(
                  (SChar*)"TRANS_LOG_COMP_RESOURCE_POOL",
                  16, // aInitialResourceCount
                  smuProperty::getMinCompressionResourceCount(),
                  smuProperty::getCompressionResourceGCSecond() )
              != IDE_SUCCESS );

    IDE_TEST( mMutexListNodePool.initialize(
                  IDU_MEM_SM_TRANSACTION_TABLE,
                  (SChar *)"MUTEX_LIST_NODE_MEMORY_POOL",
                  ID_SCALABILITY_SYS,
                  ID_SIZEOF(smuList),
                  SMX_MUTEX_LIST_NODE_POOL_SIZE,
                  IDU_AUTOFREE_CHUNK_LIMIT,			/* ChunkLimit */
                  ID_TRUE,							/* UseMutex */
                  IDU_MEM_POOL_DEFAULT_ALIGN_SIZE,	/* AlignByte */
                  ID_FALSE,							/* ForcePooling */
                  ID_TRUE,							/* GarbageCollection */
                  ID_TRUE)							/* HWCacheLine */
              != IDE_SUCCESS );

    IDE_TEST( mLobCursorPool.initialize(
                  IDU_MEM_SM_SMX,
                  (SChar *)"LOB_CURSOR_POOL",
                  ID_SCALABILITY_SYS,
                  ID_SIZEOF(smLobCursor),
                  16,/* aElemCount */
                  IDU_AUTOFREE_CHUNK_LIMIT,			/* ChunkLimit */
                  ID_TRUE,							/* UseMutex */
                  IDU_MEM_POOL_DEFAULT_ALIGN_SIZE,	/* AlignByte */
                  ID_FALSE,							/* ForcePooling */
                  ID_TRUE,							/* GarbageCollection */
                  ID_TRUE)							/* HWCacheLine */
              != IDE_SUCCESS );

    IDE_TEST( mLobColBufPool.initialize(
                  IDU_MEM_SM_SMX,
                  (SChar *)"LOB_COLUMN_BUFFER_POOL",
                  ID_SCALABILITY_SYS,
                  ID_SIZEOF(sdcLobColBuffer),
                  16,/* aElemCount */
                  IDU_AUTOFREE_CHUNK_LIMIT,			/* ChunkLimit */
                  ID_TRUE,							/* UseMutex */
                  IDU_MEM_POOL_DEFAULT_ALIGN_SIZE,	/* AlignByte */
                  ID_FALSE,							/* ForcePooling */
                  ID_TRUE,							/* GarbageCollection */
                  ID_TRUE)							/* HwCacheLine */
              != IDE_SUCCESS );

    smcLob::initializeFixedTableArea();
    sdcLob::initializeFixedTableArea();

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

IDE_RC smxTrans::destroyStatic()
{
    IDE_TEST( mMutexListNodePool.destroy() != IDE_SUCCESS );

    IDE_TEST( mCompResPool.destroy() != IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}


IDE_RC smxTrans::initialize(smTID aTransID, UInt aSlotMask)
{
    SChar sBuffer[128];
    UInt  sState = 0;

    idlOS::memset(sBuffer, 0, 128);
    idlOS::snprintf(sBuffer, 128, "TRANS_MUTEX_%"ID_UINT32_FMT, (UInt)aTransID);

    // �α� ���� ���ҽ� �ʱ�ȭ (���� �α�� Pool���� �����´� )
    mCompRes        = NULL;

    mTransID        = aTransID;
#ifdef PROJ_2181_DBG
    mTransIDDBG   = aTransID;
#endif
    mSlotN          = mTransID & aSlotMask;
    mUpdateSize     = 0;
    mLogOffset      = 0;
    SM_LSN_MAX( mBeginLogLSN );
    SM_LSN_MAX( mCommitLogLSN );
    mLegacyTransCnt = 0;
    mStatus         = SMX_TX_END;
    mStatus4FT      = SMX_TX_END;

    mOIDToVerify         = NULL;
    mOIDList             = NULL;
    mLogBuffer           = NULL;
    mCacheOIDNode4Insert = NULL;
    mReplLockTimeout     = smuProperty::getReplLockTimeOut();
    //PRJ-1476
    /* smxTrans_initialize_alloc_CacheOIDNode4Insert.tc */
    IDU_FIT_POINT("smxTrans::initialize::alloc::CacheOIDNode4Insert");
    IDE_TEST( smxOIDList::alloc(&mCacheOIDNode4Insert)
              != IDE_SUCCESS );

    sState = 1;

    /* smxTrans_initialize_malloc_OIDList.tc */
    IDU_FIT_POINT_RAISE("smxTrans::initialize::malloc::OIDList", insufficient_memory);
    IDE_TEST_RAISE( iduMemMgr::malloc( IDU_MEM_SM_SMX,
                                 ID_SIZEOF(smxOIDList),
                                 (void**)&mOIDList) != IDE_SUCCESS,
                    insufficient_memory );
    sState = 2;

    IDE_TEST( mOIDList->initialize( this,
                                   mCacheOIDNode4Insert,
                                   ID_FALSE, // aIsUnique
                                   &smxOIDList::addOIDWithCheckFlag )
              != IDE_SUCCESS );

    IDE_TEST( mOIDFreeSlotList.initialize( this,
                                           NULL,
                                           ID_FALSE, // aIsUnique
                                           &smxOIDList::addOID )
              != IDE_SUCCESS );

    /* BUG-27122 Restart Recovery �� Undo Trans�� �����ϴ� �ε����� ����
     * Integrity üũ��� �߰� (__SM_CHECK_DISK_INDEX_INTEGRITY=2) */
    if ( smuProperty::getCheckDiskIndexIntegrity()
                      == SMU_CHECK_DSKINDEX_INTEGRITY_LEVEL2 )
    {
        /* TC/FIT/Limit/sm/smx/smxTrans_initialize_malloc1.sql */
        IDU_FIT_POINT_RAISE( "smxTrans::initialize::malloc1",
                              insufficient_memory );

        IDE_TEST_RAISE( iduMemMgr::malloc( IDU_MEM_SM_SMX,
                                     ID_SIZEOF(smxOIDList),
                                     (void**)&mOIDToVerify ) != IDE_SUCCESS,
                        insufficient_memory );
        sState = 3;

        IDE_TEST( mOIDToVerify->initialize( this,
                                            NULL,
                                            ID_TRUE, // aIsUnique
                                            &smxOIDList::addOIDToVerify )
                  != IDE_SUCCESS );
    }

    IDE_TEST( mTouchPageList.initialize( this ) != IDE_SUCCESS );

    IDE_TEST( mTableInfoMgr.initialize() != IDE_SUCCESS );
    IDE_TEST( init() != IDE_SUCCESS );

    /* smxTransFreeList::alloc, free�ÿ� ID_TRUE, ID_FALSE�� �����˴ϴ�. */
    mIsFree = ID_TRUE;

    IDE_TEST( mMutex.initialize( sBuffer,
                                 IDU_MUTEX_KIND_POSIX,
                                 IDV_WAIT_INDEX_NULL ) != IDE_SUCCESS );

    idlOS::snprintf( sBuffer, 
                     ID_SIZEOF(sBuffer), 
                     "TRANS_COND_%"ID_UINT32_FMT,  
                     (UInt)aTransID);

    IDE_TEST_RAISE(mCondV.initialize(sBuffer) != IDE_SUCCESS, err_cond_var_init);

    SMU_LIST_INIT_BASE(&mMutexList);

    // �α� ���� ���� ��� �ʱ�ȭ
    mLogBufferSize = SMX_TRANSACTION_LOG_BUFFER_INIT_SIZE;

    IDE_ASSERT( SMR_LOGREC_SIZE(smrUpdateLog) < mLogBufferSize );

    /* TC/FIT/Limit/sm/smx/smxTrans_initialize_malloc2.sql */
    IDU_FIT_POINT_RAISE( "smxTrans::initialize::malloc2",
                          insufficient_memory );

    IDE_TEST_RAISE( iduMemMgr::malloc( IDU_MEM_SM_TRANSACTION_TABLE,
                                       mLogBufferSize,
                                       (void**)&mLogBuffer) != IDE_SUCCESS,
                    insufficient_memory );
    sState = 4;

    // PrivatePageList ���� ��� �ʱ�ȭ
    mPrivatePageListCachePtr = NULL;
    mVolPrivatePageListCachePtr = NULL;

    IDE_TEST( smuHash::initialize( &mPrivatePageListHashTable,
                                   1,                       // ConcurrentLevel
                                   SMX_PRIVATE_BUCKETCOUNT, // BucketCount
                                   (UInt)ID_SIZEOF(smOID),  // KeyLength
                                   ID_FALSE,                // UseLatch
                                   hash,                    // HashFunc
                                   isEQ )                   // CompFunc
             != IDE_SUCCESS );

    IDE_TEST( smuHash::initialize( &mVolPrivatePageListHashTable,
                                   1,                       // ConcurrentLevel
                                   SMX_PRIVATE_BUCKETCOUNT, // BucketCount
                                   (UInt)ID_SIZEOF(smOID),  // KeyLength
                                   ID_FALSE,                // UseLatch
                                   hash,                    // HashFunc
                                   isEQ )                   // CompFunc
             != IDE_SUCCESS );

    IDE_TEST( mPrivatePageListMemPool.initialize(
                                     IDU_MEM_SM_SMX,
                                     (SChar*)"SMP_PRIVATEPAGELIST",
                                     1,                                          // list_count
                                     (vULong)ID_SIZEOF(smpPrivatePageListEntry), // elem_size
                                     SMX_PRIVATE_BUCKETCOUNT,                    // elem_count
                                     IDU_AUTOFREE_CHUNK_LIMIT,					 // ChunkLimit
                                     ID_TRUE,									 // UseMutex
                                     IDU_MEM_POOL_DEFAULT_ALIGN_SIZE,			 // AlignByte
                                     ID_FALSE,									 // ForcePooling 
                                     ID_TRUE,									 // GarbageCollection
                                     ID_TRUE)									 // HWCacheLine
             != IDE_SUCCESS );

    IDE_TEST( mVolPrivatePageListMemPool.initialize(
                                     IDU_MEM_SM_SMX,
                                     (SChar*)"SMP_VOLPRIVATEPAGELIST",
                                     1,                                          // list_count
                                     (vULong)ID_SIZEOF(smpPrivatePageListEntry), // elem_size
                                     SMX_PRIVATE_BUCKETCOUNT,                    // elem_count
                                     IDU_AUTOFREE_CHUNK_LIMIT,					 // ChunkLimit
                                     ID_TRUE,									 // UseMutex
                                     IDU_MEM_POOL_DEFAULT_ALIGN_SIZE,			 // AlignByte
                                     ID_FALSE,									 // ForcePooling
                                     ID_TRUE,									 // GarbageCollection
                                     ID_TRUE)									 // HWCacheLine
             != IDE_SUCCESS );

    //PROJ-1362
    //fix BUG-21311
    //fix BUG-40790
    IDE_TEST( smuHash::initialize( &mLobCursorHash,
                                   1,
                                   smuProperty::getLobCursorHashBucketCount(),
                                   ID_SIZEOF(smLobCursorID),
                                   ID_FALSE,
                                   genHashValueFunc,
                                   compareFunc )
             != IDE_SUCCESS );

    /* PROJ-1594 Volatile TBS */
    /* ����� mVolatileLogEnv�� �ʱ�ȭ�Ѵ�. ID_TRUE�� align force ��. */
    IDE_TEST( svrLogMgr::initEnv(&mVolatileLogEnv, ID_TRUE ) != IDE_SUCCESS );

    mDiskTBSAccessed   = ID_FALSE;
    mMemoryTBSAccessed = ID_FALSE;
    mMetaTableModified = ID_FALSE;

    /* PROJ-2162 RestartRiskReduction */
    smrRecoveryMgr::initRTOI( & mRTOI4UndoFailure );

    return IDE_SUCCESS;

    IDE_EXCEPTION(err_cond_var_init);
    {
        IDE_SET(ideSetErrorCode(smERR_FATAL_ThrCondInit));
    }
    IDE_EXCEPTION( insufficient_memory );
    {
        IDE_SET(ideSetErrorCode(idERR_ABORT_InsufficientMemory));
    }
    IDE_EXCEPTION_END;

    IDE_PUSH();
    {
        switch( sState )
        {
        case 4:
            if ( mLogBuffer != NULL )
            {
                IDE_ASSERT( iduMemMgr::free(mLogBuffer) == IDE_SUCCESS );
                mLogBuffer = NULL;
            }
        case 3:
            if ( mOIDToVerify != NULL )
            {
                IDE_ASSERT( iduMemMgr::free(mOIDToVerify) == IDE_SUCCESS );
                mOIDToVerify = NULL;
            }
        case 2:
            if ( mOIDList != NULL )
            {
                IDE_ASSERT( iduMemMgr::free(mOIDList) == IDE_SUCCESS );
                mOIDList = NULL;
            }
        case 1:
            if ( mCacheOIDNode4Insert != NULL )
            {
                IDE_ASSERT( smxOIDList::freeMem(mCacheOIDNode4Insert)
                            == IDE_SUCCESS );
                mCacheOIDNode4Insert = NULL;
            }
        case 0:
            break;
        default:
            /* invalid case */
            IDE_ASSERT( 0 );
            break;
        }
    }
    IDE_POP();

    return IDE_FAILURE;
}

IDE_RC smxTrans::destroy()
{
    IDE_ASSERT( mStatus == SMX_TX_END );

    if ( mOIDToVerify != NULL )
    {
        IDE_TEST( mOIDToVerify->destroy() != IDE_SUCCESS );
        IDE_TEST( iduMemMgr::free(mOIDToVerify) != IDE_SUCCESS );
        mOIDToVerify = NULL;
    }

    /* PROJ-1381 Fetch Across Commits
     * smxTrans::destroy �Լ��� ���� �� ȣ�� �� �� �����Ƿ�
     * mOIDList�� NULL�� �ƴ� ���� OID List�� �����ϵ��� �Ѵ�. */
    if ( mOIDList != NULL )
    {
        IDE_TEST( mOIDList->destroy() != IDE_SUCCESS );
        IDE_TEST( iduMemMgr::free(mOIDList) != IDE_SUCCESS );
        mOIDList = NULL;
    }

    IDE_TEST( mOIDFreeSlotList.destroy()!= IDE_SUCCESS );

    /* PROJ-1362 */
    IDE_TEST( smuHash::destroy(&mLobCursorHash) != IDE_SUCCESS );

    /* PrivatePageList���� ��� ���� */
    IDE_DASSERT( mPrivatePageListCachePtr == NULL );
    IDE_DASSERT( mVolPrivatePageListCachePtr == NULL );

    IDE_TEST( smuHash::destroy(&mPrivatePageListHashTable) != IDE_SUCCESS );
    IDE_TEST( smuHash::destroy(&mVolPrivatePageListHashTable) != IDE_SUCCESS );

    IDE_TEST( mTouchPageList.destroy() != IDE_SUCCESS );

    IDE_TEST( mPrivatePageListMemPool.destroy() != IDE_SUCCESS );
    IDE_TEST( mVolPrivatePageListMemPool.destroy() != IDE_SUCCESS );

    /* PROJ-1594 Volatile TBS */
    IDE_TEST( svrLogMgr::destroyEnv(&mVolatileLogEnv) != IDE_SUCCESS );

    IDE_TEST( mMutex.destroy() != IDE_SUCCESS );

    IDE_ASSERT( mLogBuffer != NULL );
    IDE_TEST( iduMemMgr::free(mLogBuffer) != IDE_SUCCESS );
    mLogBuffer = NULL;

    IDE_TEST( mTableInfoMgr.destroy() != IDE_SUCCESS );

    IDE_ASSERT( mCacheOIDNode4Insert != NULL );

    smxOIDList::freeMem(mCacheOIDNode4Insert);
    mCacheOIDNode4Insert = NULL;

    IDE_TEST_RAISE(mCondV.destroy() != IDE_SUCCESS,
                   err_cond_var_destroy);

    return IDE_SUCCESS;

    IDE_EXCEPTION(err_cond_var_destroy);
    {
        IDE_SET(ideSetErrorCode(smERR_FATAL_ThrCondDestroy));
    }
    IDE_EXCEPTION_END;

    return IDE_FAILURE;

}

/* TASK-2398 �α׾���
   Ʈ������� �α� ����/���������� ����� ���ҽ��� �����Ѵ�

   [OUT] aCompRes - �α� ���� ���ҽ�
*/
IDE_RC smxTrans::getCompRes(smrCompRes ** aCompRes)
{
    if ( mCompRes == NULL ) // Transaction Begin���� ó�� ȣ��� ���
    {
        // Log ���� ���ҽ��� Pool���� �����´�.
        IDE_TEST( mCompResPool.allocCompRes( & mCompRes )
                  != IDE_SUCCESS );
    }

    IDE_DASSERT( mCompRes != NULL );

    *aCompRes = mCompRes;

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/* Ʈ������� �α� ����/���������� ����� ���ҽ��� �����Ѵ�

   [IN] aTrans - Ʈ�����
   [OUT] aCompRes - �α� ���� ���ҽ�
 */
IDE_RC smxTrans::getCompRes4Callback( void *aTrans, smrCompRes ** aCompRes )
{
    IDE_DASSERT( aTrans != NULL );
    IDE_DASSERT( aCompRes != NULL );

    smxTrans * sTrans = (smxTrans *) aTrans;

    IDE_TEST( sTrans->getCompRes( aCompRes )
              != IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

IDE_RC smxTrans::init()
{
    mIsUpdate        = ID_FALSE;

    /*PROJ-1541 Eager Replication
     *mReplMode�� Session�� Eager,Acked,Lazy�� ������ �� �ֵ���
     *�ϱ� ���� �����ϸ�, DEFAULT ���� ���´�.
     */

    mFlag = 0;
    mFlag = SMX_REPL_DEFAULT | SMX_COMMIT_WRITE_NOWAIT;

    SM_LSN_MAX( mLastWritedLSN );
    // For Global Transaction
    initXID();

    mCommitState          = SMX_XA_COMPLETE;
    mPreparedTime.tv_sec  = (long)0;
    mPreparedTime.tv_usec = (long)0;

    SMU_LIST_INIT_BASE(&mPendingOp);

    mUpdateSize           = 0;

    /* PROJ-1381 Fetch Across Commits
     * Legacy Trans�� ������ �Ʒ��� ����� �ʱ�ȭ ���� �ʴ´�.
     * MinViewSCNs       : aging ����
     * mFstUndoNxtLSN    : ������ ����� redo������ ����
     * initTransLockList : IS Lock ���� */
    if ( mLegacyTransCnt == 0 )
    {
        mStatus    = SMX_TX_END;
        mStatus4FT = SMX_TX_END;

        SM_SET_SCN_INFINITE( &mMinMemViewSCN );
        SM_SET_SCN_INFINITE( &mMinDskViewSCN );
        SM_SET_SCN_INFINITE( &mFstDskViewSCN );

        // BUG-26881 �߸��� CTS stamping���� access�� �� ���� row�� ������
        SM_SET_SCN_INFINITE( &mOldestFstViewSCN );

        SM_LSN_MAX(mFstUndoNxtLSN);

        /* initialize lock slot */
        smLayerCallback::initTransLockList( mSlotN );
    }

    SM_SET_SCN_INFINITE( &mCommitSCN );

    mLogTypeFlag = SMR_LOG_TYPE_NORMAL;

    SM_LSN_MAX(mLstUndoNxtLSN);

    mLSLockFlag = SMX_TRANS_UNLOCKED;
    mDoSkipCheckSCN = ID_FALSE;
    mAbleToRollback = ID_TRUE;

    //For Session Management
    mFstUpdateTime       = 0;
    mLogOffset           = 0;

    /* BUG-33895 [sm_recovery] add the estimate function 
     * the time of transaction undoing. */
    mUndoBeginTime         = 0;
    mTotalLogCount     = 0;
    mProcessedUndoLogCount = 0;
    // PROJ-1362 QP Large Record & Internal LOB
    mCurLobCursorID = 0;
    //fix BUG-21311
    mMemLCL.initialize();
    mDiskLCL.initialize();

    mTableInfoPtr  = NULL;
    mDoSkipCheck   = ID_FALSE;
    mIsDDL         = ID_FALSE;
    mIsFirstLog    = ID_TRUE;
    mIsTransWaitRepl = ID_FALSE;

    mTXSegEntryIdx = ID_UINT_MAX;
    mTXSegEntry    = NULL;

    // initialize PageListID
    mRSGroupID = SMP_PAGELISTID_NULL;

    IDE_TEST( mTableInfoMgr.init() != IDE_SUCCESS );

    /* Disk Insert Rollback (Partial Rollback ����)�� Flag�� FALSE��
       �Ͽ�, Commit �̳� Abort�ÿ� Aging List�� �߰��Ҽ� �ְ� �Ѵ�. */
    mFreeInsUndoSegFlag = ID_TRUE;

    /* TASK-2401 MMAP Loggingȯ�濡�� Disk/Memory Log�� �и�
       Disk/Memory Table�� �����ߴ��� ���θ� �ʱ�ȭ
     */
    mDiskTBSAccessed   = ID_FALSE;
    mMemoryTBSAccessed = ID_FALSE;
    mMetaTableModified = ID_FALSE;

    // PROJ-2068
    mDPathEntry = NULL;

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;

}

/***********************************************************************
 * Description: 1. aWaitTrans != NULL
 *                 aWaitTrans �� ��� �ִ� Resource�� Free�ɶ����� ��ٸ���.
 *                 ������ Transaction�� �ٸ� event(query timeout, session
 *                 timeout)�� ���ؼ� �������� �ʴ� �̻� aWaitMicroSec��ŭ
 *                 ����Ѵ�.
 *
 *              2. aMutex != NULL
 *                 Record Lock���� ���� Page Mutex�� ��� Lock Validation��
 *                 �����Ѵ�. �� Mutex�� Waiting�ϱ����� Ǯ��� �Ѵ�. ���⼭
 *                 Ǯ�� �ٽ� Transaction�� ����� ��쿡 �ٽ� Mutex�� ���
 *                 ������ �ش� Record�� Lock�� Ǯ�ȴ��� �˻��Ѵ�.
 *
 * aWaitTrans    - [IN] �ش� Resource�� ������ �ִ� Transaction
 * aMutex        - [IN] �ش� Resource( ex: Record)�� �������� Ʈ������� ���ÿ�
 *                      �����ϴ°����κ��� ��ȣ�ϴ� Mutex
 * aWaitMicroSec - [IN] �󸶳� ��ٷ��� ������ ����.
 * */
IDE_RC smxTrans::suspendMutex(smxTrans * aWaitTrans,
                              iduMutex * aMutex,
                              ULong      aWaitMicroSec)
{
    idBool            sWaited          = ID_FALSE;
    idBool            sMyMutexLocked   = ID_FALSE;
    idBool            sTxMutexUnlocked = ID_FALSE;
    ULong             sWaitSec         = 0;
    ULong             sMustWaitSec     = 0;
    PDL_Time_Value    sTimeVal;

    IDE_DASSERT(smuProperty::getLockMgrType() == 0);

    /*
     * Check whether this TX is waiting for itself
     * ASSERT in debug mode
     * Deadlock warning in release mode
     */
    if( aWaitTrans != NULL )
    {
        IDE_DASSERT( mSlotN != aWaitTrans->mSlotN );
        IDE_TEST_RAISE( mSlotN == aWaitTrans->mSlotN , err_selflock );
    }
    else
    {
        /* fall through */
    }

    if( aWaitMicroSec != 0 )
    {
        /* Micro�� Sec�� ��ȯ�Ѵ�. */
        sMustWaitSec = aWaitMicroSec / 1000000;
    }
    else
    {
        idlOS::thr_yield();
    }

    IDE_TEST( lock() != IDE_SUCCESS );
    sMyMutexLocked = ID_TRUE;

    if ( aWaitTrans != NULL )
    {
        IDE_ASSERT( aMutex == NULL );

        if ( smLayerCallback::didLockReleased( mSlotN, aWaitTrans->mSlotN )
             == ID_TRUE )
        {
            sMyMutexLocked = ID_FALSE;
            IDE_TEST( mMutex.unlock() != IDE_SUCCESS );

            return IDE_SUCCESS;
        }
    }
    else
    {
        sTxMutexUnlocked = ID_TRUE;
        IDE_TEST_RAISE( aMutex->unlock() != IDE_SUCCESS,
                        err_mutex_unlock );
    }

    if ( mStatus == SMX_TX_BLOCKED )
    {
        IDE_TEST( smLayerCallback::lockWaitFunc( aWaitMicroSec, &sWaited )
                  != IDE_SUCCESS );
    }

    while( mStatus == SMX_TX_BLOCKED )
    {
        /* BUG-18965: LOCK TABLE�������� QUERY_TIMEOUT�� ��������
         * �ʽ��ϴ�.
         *
         * ����ڰ� LOCK TIME OUT�� �����ϰ� �Ǹ� ������ LOCK TIME
         * OUT������ ������ ��ٷȴ�. ������ QUERY TIMEOUT, SESSION
         * TIMEOUT�� üũ���� �ʾƼ� ���� ������ ���� ���ϰ� LOCK
         * TIMEOUT������ ��ٷ��� �Ѵ�. �̷� ������ �ذ��ϱ� ���ؼ�
         * �ֱ������� ���� mStatistics�� ��ٸ� �ð��� LOCK_TIMEOUT��
         * �Ѿ����� �˻��Ѵ�.
         * */
        if ( sMustWaitSec == 0 )
        {
            break;
        }

        sWaitSec = sMustWaitSec < 3 ? sMustWaitSec : 3;
        sMustWaitSec -= sWaitSec;

        sTimeVal.set( idlOS::time(NULL) + sWaitSec, 0 );

        IDE_TEST_RAISE(mCondV.timedwait(&mMutex, &sTimeVal, IDU_IGNORE_TIMEDOUT)
                       != IDE_SUCCESS, err_cond_wait);
        IDE_TEST( iduCheckSessionEvent( mStatistics )
                  != IDE_SUCCESS );
    }

    if ( sWaited == ID_TRUE )
    {
        IDE_TEST( smLayerCallback::lockWakeupFunc() != IDE_SUCCESS );
    }

    sMyMutexLocked = ID_FALSE;
    IDE_TEST( mMutex.unlock() != IDE_SUCCESS );

    if ( sTxMutexUnlocked == ID_TRUE )
    {
        sTxMutexUnlocked = ID_FALSE;
        IDE_TEST_RAISE( aMutex->lock( NULL ) != IDE_SUCCESS,
                       err_mutex_lock);
    }
    return IDE_SUCCESS;

    IDE_EXCEPTION(err_selflock);
    {
        ideLog::log(IDE_SM_0, SM_TRC_WAIT_SELF_WARNING, mSlotN);
        IDE_SET(ideSetErrorCode(smERR_ABORT_Aborted));
    }

    IDE_EXCEPTION(err_mutex_unlock);
    {
        IDE_SET(ideSetErrorCode (smERR_FATAL_ThrMutexUnlock));
    }
    IDE_EXCEPTION(err_cond_wait);
    {
        IDE_SET(ideSetErrorCode(smERR_FATAL_ThrCondWait));
    }
    IDE_EXCEPTION(err_mutex_lock);
    {
        IDE_SET(ideSetErrorCode (smERR_FATAL_ThrMutexLock));
    }
    IDE_EXCEPTION_END;

    if ( sMyMutexLocked ==  ID_TRUE )
    {
        IDE_ASSERT( mMutex.unlock() == IDE_SUCCESS );
    }

    if ( sTxMutexUnlocked == ID_TRUE )
    {
        IDE_ASSERT( aMutex->lock( NULL ) == IDE_SUCCESS );
    }

    // fix BUG-11228.
    if (sWaited == ID_TRUE )
    {
        IDE_ASSERT( smLayerCallback::lockWakeupFunc() == IDE_SUCCESS );
    }

    return IDE_FAILURE;
}

/***********************************************************************
 * Description: 1. aWaitTrans != NULL
 *                 aWaitTrans �� ��� �ִ� Resource�� Free�ɶ����� ��ٸ���.
 *                 ������ Transaction�� �ٸ� event(query timeout, session
 *                 timeout)�� ���ؼ� �������� �ʴ� �̻� aWaitMicroSec��ŭ
 *                 ����Ѵ�.
 *
 *              2. aMutex != NULL
 *                 Record Lock���� ���� Page Mutex�� ��� Lock Validation��
 *                 �����Ѵ�. �� Mutex�� Waiting�ϱ����� Ǯ��� �Ѵ�. ���⼭
 *                 Ǯ�� �ٽ� Transaction�� ����� ��쿡 �ٽ� Mutex�� ���
 *                 ������ �ش� Record�� Lock�� Ǯ�ȴ��� �˻��Ѵ�.
 *
 * aWaitTrans    - [IN] �ش� Resource�� ������ �ִ� Transaction
 * aWaitMicroSec - [IN] �󸶳� ��ٷ��� ������ ����.
 * */
IDE_RC smxTrans::suspendSpin(smxTrans * aWaitTrans,
                             smTID      aWaitTransID,
                             ULong      aWaitMicroSec)
{
    acp_time_t sBeginTime = acpTimeNow();
    acp_time_t sCurTime;
    IDE_DASSERT(smuProperty::getLockMgrType() == 1);

    /*
     * Check whether this TX is waiting for itself
     * ASSERT in debug mode
     * Deadlock warning in release mode
     */
    IDE_DASSERT( mSlotN != aWaitTrans->mSlotN );
    IDE_TEST_RAISE( mSlotN == aWaitTrans->mSlotN , err_selflock );

    smlLockMgr::beginPending(mSlotN);
    smlLockMgr::incTXPendCount();

    do
    {
        if ( ( smLayerCallback::didLockReleased( mSlotN, aWaitTrans->mSlotN ) == ID_TRUE ) ||
             ( aWaitTrans->mTransID != aWaitTransID ) ||
             ( aWaitTrans->mStatus  == SMX_TX_END ) )
        {
            mStatus     = SMX_TX_BEGIN;
            mStatus4FT  = SMX_TX_BEGIN;
            break;
        }
        else
        {
            /* fall through */
        }

        IDE_TEST_RAISE( smlLockMgr::isCycle(mSlotN) == ID_TRUE, err_deadlock );
        IDE_TEST( iduCheckSessionEvent( mStatistics ) != IDE_SUCCESS );
        idlOS::thr_yield();
        sCurTime = acpTimeNow();
    } while( (ULong)(sCurTime - sBeginTime) < aWaitMicroSec );

    smlLockMgr::decTXPendCount();
    return IDE_SUCCESS;

    IDE_EXCEPTION(err_selflock);
    {
        ideLog::log(IDE_SM_0, SM_TRC_WAIT_SELF_WARNING, mSlotN);
        IDE_SET(ideSetErrorCode(smERR_ABORT_Aborted));
    }

    IDE_EXCEPTION(err_deadlock);
    {
        IDE_SET(ideSetErrorCode(smERR_ABORT_Aborted));
    }

    IDE_EXCEPTION_END;
    smlLockMgr::decTXPendCount();
    return IDE_FAILURE;
}

IDE_RC smxTrans::resume()
{

    UInt sState = 0;

    IDE_TEST( lock() != IDE_SUCCESS );
    sState = 1;

    if( mStatus == SMX_TX_BLOCKED )
    {
        mStatus    = SMX_TX_BEGIN;
        mStatus4FT = SMX_TX_BEGIN;

        IDE_TEST_RAISE(mCondV.signal() != IDE_SUCCESS, err_cond_signal);
        //fix bug-9627.
    }
    else
    {
        /* BUG-43595 ���� alloc�� transaction ��ü�� state��
         * begin�� ��찡 �ֽ��ϴ�. �� ���� ����� ���� ��� �߰�*/
        ideLog::log(IDE_ERR_0,"Resume error, Transaction is not blocked.\n");
        dumpTransInfo();
        ideLog::logCallStack(IDE_ERR_0);
        IDE_DASSERT(0);
    }

    sState = 0;
    IDE_TEST( unlock() != IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION(err_cond_signal);
    {
        IDE_SET(ideSetErrorCode(smERR_FATAL_ThrCondSignal));
    }
    IDE_EXCEPTION_END;

    if ( sState != 0 )
    {
        IDE_ASSERT( unlock() == IDE_SUCCESS );
    }

    return IDE_FAILURE;

}


IDE_RC smxTrans::begin( idvSQL * aStatistics,
                        UInt     aFlag,
                        UInt     aReplID )
{
    UInt sState = 0;
    UInt sFlag  = 0;

    mReplLockTimeout = smuProperty::getReplLockTimeOut();
    /* mBeginLogLSN�� CommitLogLSN�� recovery from replication����
     * ����ϸ�, commit�� ȣ��� ���Ŀ� ���ǹǷ�, �ݵ��
     * begin���� �ʱ�ȭ �Ͽ��� �ϸ�, init�Լ��� commit�� ��
     * ȣ��ǹǷ� init���� �ʱ�ȭ �ϸ� �ȵȴ�.
     */
    SM_LSN_MAX( mBeginLogLSN );
    SM_LSN_MAX( mCommitLogLSN );

    SM_SET_SCN_INFINITE_AND_TID( &mInfinite, mTransID );

    IDE_ASSERT( mCommitState == SMX_XA_COMPLETE );

    /* PROJ-1381 Fetch Across Commits
     * SMX_TX_END�� �ƴ� TX�� MinViewSCN �� ���� ���� ������ aging�ϹǷ�,
     * Legacy Trans�� ������ mStatus�� SMX_TX_END�� �����ؼ��� �ȵȴ�.
     * SMX_TX_END ���·� �����ϸ� ���������� cursor�� view�� ���� row��
     * aging ����� �Ǿ� ����� �� �ִ�. */
    if ( mLegacyTransCnt == 0 )
    {
        IDE_ASSERT( mStatus == SMX_TX_END );
    }

    // PROJ-2068 Begin�� DPathEntry�� NULL�̾�� �Ѵ�.
    IDE_DASSERT( mDPathEntry == NULL );

    /*
     * BUG-42927
     * mEnabledTransBegin == ID_FALSE �̸�, smxTrans::begin()�� ����ϵ��� �Ѵ�.
     *
     * �ֳ��ϸ�,
     * non-autocommit Ʈ�������� ���� statement ���۽�
     * smxTransMgr::alloc()�� ȣ������ �ʰ�, smxTrans::begin()�� ȣ���ϱ� �����̴�.
     */
    while ( 1 )
    {
        if ( smxTransMgr::mEnabledTransBegin == ID_TRUE )
        {
            break;
        }
        else
        {
            idlOS::thr_yield();
        }
    }

    mOIDList->init();
    mOIDFreeSlotList.init();


    mSvpMgr.initialize( this );
    mStatistics = aStatistics;
    //fix BUG-23656 session,xid ,transaction�� ������ performance view�� �����ϰ�,
    //�׵鰣�� ���踦 ��Ȯ�� �����ؾ� ��.
    // transaction begin�� session id�� ������.
    if ( aStatistics != NULL )
    {
        if ( aStatistics->mSess != NULL )
        {
            mSessionID = aStatistics->mSess->mSID;
        }
        else
        {
            mSessionID = ID_NULL_SESSION_ID;
        }
    }
    else
    {
        mSessionID = ID_NULL_SESSION_ID;
    }

    // Disk Ʈ������� ���� Touch Page List �ʱ�ȭ
    mTouchPageList.init( aStatistics );

    IDE_TEST( lock() != IDE_SUCCESS );
    sState = 1;

    mStatus    = SMX_TX_BEGIN;
    mStatus4FT = SMX_TX_BEGIN;

    // To Fix BUG-15396
    // mFlag���� ���� ������ ������ ������
    // (1) transaction replication mode
    // (2) commit write wait mode
    mFlag = aFlag;

    // For XA: There is no sense for local transaction
    mCommitState = SMX_XA_START;

    //PROJ-1541 eager replication Flag Set
    /* PROJ-1608 Recovery From Replication
     * SMX_REPL_NONE(replication���� �ʴ� Ʈ�����-system Ʈ�����) OR
     * SMX_REPL_REPLICATED(recovery�� �������� �ʴ� receiver�� ������ Ʈ�����)�� ��쿡
     * �α׸� Normal Sender�� �� �ʿ䰡 �����Ƿ�, SMR_LOG_TYPE_REPLICATED�� ����
     * �׷��� �ʰ� SMX_REPL_RECOVERY(repl recovery�� �����ϴ� receiver�� ������ Ʈ�����)��
     * ��� SMR_LOG_TYPE_REPL_RECOVERY�� �����Ͽ� �α׸� ���� ��,
     * Recovery Sender�� �� �� �ֵ��� RP�� ���� ������ ���� �� �ֵ��� �Ѵ�.
     */
    sFlag = mFlag & SMX_REPL_MASK;

    if ( ( sFlag == SMX_REPL_REPLICATED ) || ( sFlag == SMX_REPL_NONE ) )
    {
        mLogTypeFlag = SMR_LOG_TYPE_REPLICATED;
    }
    else
    {
        if ( ( sFlag == SMX_REPL_RECOVERY ) )
        {
            mLogTypeFlag = SMR_LOG_TYPE_REPL_RECOVERY;
        }
        else
        {
            mLogTypeFlag = SMR_LOG_TYPE_NORMAL;
        }
    }
    // PROJ-1553 Replication self-deadlock
    // tx�� begin�� ��, replication�� ���� tx�� ���
    // mReplID�� �޴´�.
    mReplID = aReplID;
    //PROJ-1541 Eager/Acked replication
    SM_LSN_MAX( mLastWritedLSN );

    sState = 0;
    IDE_TEST( unlock() != IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    if ( sState != 0 )
    {
        IDE_ASSERT( unlock() == IDE_SUCCESS );
    }

    return IDE_FAILURE;
}

IDE_RC smxTrans::commit( smSCN  * aCommitSCN,
                         idBool   aIsLegacyTrans,
                         void  ** aLegacyTrans )
{
    smLSN           sCommitEndLSN   = {0, 0};
    sdcTSSegment  * sCTSegPtr;
    idBool          sWriteCommitLog = ID_FALSE;
    smxOIDList    * sOIDList        =  NULL;
    UInt            sState          = 0;
    idBool          sNeedWaitTransForRepl = ID_FALSE;
    idvSQL        * sTempStatistics       = NULL;
    smTID           sTempTransID          = SM_NULL_TID;
    smLSN           sTempLastWritedLSN;
    
    IDU_FIT_POINT("1.smxTrans::smxTrans:commit");

    IDE_DASSERT( aCommitSCN != NULL );

    mStatus4FT = SMX_TX_COMMIT;

    // PROJ-2068 Direct-Path INSERT�� �������� ��� commit �۾��� �����Ѵ�.
    if ( mDPathEntry != NULL )
    {
        IDE_TEST( sdcDPathInsertMgr::commit( mStatistics,
                                             this,
                                             mDPathEntry )
                  != IDE_SUCCESS );

        IDE_TEST( mTableInfoMgr.processAtDPathInsCommit() != IDE_SUCCESS );
    }

    if ( mIsUpdate == ID_TRUE )
    {
        IDU_FIT_POINT( "1.BUG-23648@smxTrans::commit" );

        /* ������ ������ Transaction�� ��� */
        /* BUG-26482 ��� �Լ��� CommitLog ��� ���ķ� �и��Ͽ� ȣ���մϴ�. */
        if ( ( mIsTransWaitRepl == ID_TRUE ) && ( isReplTrans() == ID_FALSE ) )
        {
            if (smrRecoveryMgr::mIsReplCompleteBeforeCommitFunc != NULL )
            {
                IDE_TEST( smrRecoveryMgr::mIsReplCompleteBeforeCommitFunc(
                                                     mStatistics,
                                                     mTransID,
                                                     SM_MAKE_SN(mLastWritedLSN),
                                                     ( mFlag & SMX_REPL_MASK ))
                         != IDE_SUCCESS );
            }
            else
            {
                /* do nothing */
            }

            sNeedWaitTransForRepl = ID_TRUE;
        }
        else
        {
            /* do nothing */
        }
        
        /* FIT PROJ-2569 before commit logging and communicate */
        IDU_FIT_POINT( "smxTrans::commit::writeCommitLog::BEFORE_WRITE_COMMIT_LOG" );
        
        /* Commit Log�� ����Ѵ� */
        /* BUG-32576  [sm_transaction] The server does not unlock
         * PageListMutex if the commit log writing occurs an exception.
         * CommitLog�� ���� �� ���ܰ� �߻��ϸ� PageListMutex�� Ǯ���� ��
         * ���ϴ�. �� �ܿ��� CommitLog�� ���ٰ� ���ܰ� �߻��ϸ� DB��
         * �߸��� �� �ִ� ������ �ֽ��ϴ�. */
        IDE_TEST( writeCommitLog( &sCommitEndLSN ) != IDE_SUCCESS );
        sWriteCommitLog = ID_TRUE;
        
        /* FIT PROJ-2569 After commit logging and communicate */
        IDU_FIT_POINT( "smxTrans::commit::writeCommitLog::AFTER_WRITE_COMMIT_LOG" );
        
        /* PROJ-1608 recovery from replication Commit Log LSN Set */
        SM_GET_LSN( mCommitLogLSN, mLastWritedLSN );
    
        /* Commit Log�� Flush�ɶ����� ��ٸ��� ���� ��� */
        IDE_TEST( flushLog( &sCommitEndLSN, ID_TRUE /* When Commit */ )
                  != IDE_SUCCESS );
    }
    else
    {
        if ( svrLogMgr::isOnceUpdated(&mVolatileLogEnv) == ID_TRUE )
        {
            /* �ٸ� TBS�� ������ ���� Volatile TBS���� ������ �ִ� ���
               ���ڵ� ī��Ʈ�� �������Ѿ� �Ѵ�. */
            IDE_TEST( mTableInfoMgr.requestAllEntryForCheckMaxRow()
                      != IDE_SUCCESS );

            IDE_TEST( mTableInfoMgr.releaseEntryAndUpdateMemTableInfoForIns()
                      != IDE_SUCCESS );
        }
    }

    /* Tx's PrivatePageList�� �����Ѵ�.
       �ݵ�� ������ ���� commit�α׸� write�ϰ� �۾��Ѵ�. */
    IDE_TEST( finAndInitPrivatePageList() != IDE_SUCCESS );

    /* PROJ-1594 Volatile TBS */
    /* Volatile TBS�� private page list�� �����Ѵ�. */
    IDE_TEST( finAndInitVolPrivatePageList() != IDE_SUCCESS );

    /* BUG-14093 Ager Tx�� FreeSlot�� �͵��� ���� FreeSlotList��
     * �Ŵܴ�. */
    IDE_TEST( smLayerCallback::processFreeSlotPending( this,
                                                       &mOIDFreeSlotList )
              != IDE_SUCCESS );
    
    /* ������ ������ Transaction�� ��� */
    /* BUG-26482 ��� �Լ��� CommitLog ��� ���ķ� �и��Ͽ� ȣ���մϴ�.
     * BUG-35452 lock ���� commit log�� standby�� �ݿ��� ���� Ȯ���ؾ��Ѵ�. */
    /* BUG-42736 Loose Replication ���� Remote ������ �ݿ��� ��� �Ͽ��� �մϴ�. */
    if ( sNeedWaitTransForRepl == ID_TRUE )
    {
        if ( smrRecoveryMgr::mIsReplCompleteAfterCommitFunc != NULL )
        {
            smrRecoveryMgr::mIsReplCompleteAfterCommitFunc(
                                                    mStatistics,
                                                    mTransID,
                                                    SM_MAKE_SN(mLastWritedLSN),
                                                    ( mFlag & SMX_REPL_MASK ),
                                                    SMI_BEFORE_LOCK_RELEASE);
        }
    }
    else
    {
        /* do nothing */
    }
    
    /* ovl�� �ִ� OID����Ʈ�� lock�� SCN�� �����ϰ�
     * pol�� ovl�� logical ager���� �ѱ��. */
    if ( (mTXSegEntry != NULL ) ||
         (mOIDList->isEmpty() == ID_FALSE) )
    {
        /* PROJ-1381 Fetch Across Commits
         * Legacy Trans�̸� OID List�� Logical Ager�� �߰��� �ϰ�,
         * Commit ���� ������ Legacy Trans�� ���� �� ó���Ѵ�. */
        IDE_TEST( addOIDList2AgingList( SM_OID_ACT_AGING_COMMIT,
                                        SMX_TX_COMMIT,
                                        &sCommitEndLSN,
                                        aCommitSCN,
                                        aIsLegacyTrans )
                  != IDE_SUCCESS );
    }
    else
    {
        mStatus = SMX_TX_COMMIT;
        //fux BUG-27468 ,Code-Sonar mmqQueueInfo::wakeup���� aCommitSCN UMR
        SM_INIT_SCN(aCommitSCN);
    }

    /* PRJ-1704 Disk MVCC Renewal */
    if ( mTXSegEntry != NULL )
    {
        /*
         * [BUG-27542] [SD] TSS Page Allocation ���� �Լ�(smxTrans::allocTXSegEntry,
         *             sdcTSSegment::bindTSS)���� Exceptionó���� �߸��Ǿ����ϴ�.
         */
        if ( mTXSegEntry->mTSSlotSID != SD_NULL_SID )
        {
            sCTSegPtr = sdcTXSegMgr::getTSSegPtr( mTXSegEntry );

            IDE_TEST( sCTSegPtr->unbindTSS4Commit( mStatistics,
                                                   mTXSegEntry->mTSSlotSID,
                                                   &mCommitSCN )
                      != IDE_SUCCESS );

            /* BUG-29280 - non-auto commit D-Path Insert ������
             *             rollback �߻��� ��� commit �� ���� �״� ����
             *
             * DPath INSERT ������ Fast Stamping�� �������� �ʴ´�. */
            if ( mDPathEntry == NULL )
            {
                IDE_TEST( mTouchPageList.runFastStamping( aCommitSCN )
                          != IDE_SUCCESS );
            }

            /*
             * Ʈ����� Commit�� CommitSCN�� �ý������κ��� �����ϱ� ������
             * Commit�α׿� UnbindTSS ������ atomic�ϰ� ó���� �� ����.
             * �׷��Ƿ�, Commit �α��Ŀ� unbindTSS�� �����ؾ��Ѵ�.
             * ������ ���� Commit �α�ÿ� TSS�� ���ؼ� �������
             * initSCN�� �����ϴ� �α׸� �����־�� ���� Restart�ÿ�
             * Commit�� TSS�� InfinteSCN�� ������ ������ �߻����� �ʴ´�.
             */
            IDE_TEST( sdcTXSegMgr::markSCN(
                          mStatistics,
                          mTXSegEntry,
                          aCommitSCN ) != IDE_SUCCESS );
        }

        sdcTXSegMgr::freeEntry( mTXSegEntry,
                                ID_TRUE /* aMoveToFirst */ );
        mTXSegEntry = NULL;
    }

    /* ��ũ �����ڸ� ���� pending operation���� ���� */
    IDU_FIT_POINT( "2.PROJ-1548@smxTrans::commit" );

    IDE_TEST( executePendingList( ID_TRUE ) != IDE_SUCCESS );

    IDU_FIT_POINT( "3.PROJ-1548@smxTrans::commit" );

    /* PROJ-1594 Volatile TBS */
    if (( mIsUpdate == ID_TRUE ) ||
        ( svrLogMgr::isOnceUpdated( &mVolatileLogEnv )
          == ID_TRUE ))
    {
        IDE_TEST( mTableInfoMgr.updateMemTableInfoForDel()
                  != IDE_SUCCESS );
    }

    /* PROJ-1594 Volatile TBS */
    /* commit�ÿ� �α��� ��� �α׵��� �����. */
    if ( svrLogMgr::isOnceUpdated( &mVolatileLogEnv ) == ID_TRUE )
    {
        IDE_TEST( svrLogMgr::removeLogHereafter(
                             &mVolatileLogEnv,
                             SVR_LSN_BEFORE_FIRST )
                  != IDE_SUCCESS );
    }

    /* PROJ-1381 Fetch Across Commits - Legacy Trans�� List�� �߰��Ѵ�. */
    if ( aIsLegacyTrans == ID_TRUE )
    {
        IDE_TEST( smxLegacyTransMgr::addLegacyTrans( this,  
                                                     sCommitEndLSN,
                                                     aLegacyTrans )
                  != IDE_SUCCESS );

        /* PROJ-1381 Fetch Across Commits
         * smxOIDList�� Legacy Trans�� �޾��־����Ƿ�,
         * ���ο� smxOIDList�� Ʈ�����ǿ� �Ҵ��Ѵ�.
         *
         * Memory �Ҵ翡 �����ϸ� �׳� ���� ó���Ѵ�.
         * trunk������ ���� ó���� �ٷ� ASSERT�� �����Ѵ�. */
        /* smxTrans_commit_malloc_OIDList.tc */
        IDU_FIT_POINT("smxTrans::commit::malloc::OIDList");
        IDE_TEST( iduMemMgr::malloc( IDU_MEM_SM_SMX,
                                     ID_SIZEOF(smxOIDList),
                                     (void**)&sOIDList )
                  != IDE_SUCCESS );
        sState = 1;

        IDE_TEST( sOIDList->initialize( this,
                                        mCacheOIDNode4Insert,
                                        ID_FALSE, // aIsUnique
                                        &smxOIDList::addOIDWithCheckFlag )
                  != IDE_SUCCESS );

        mOIDList = sOIDList;

        mLegacyTransCnt++;
    }

    sTempStatistics   = mStatistics;
    sTempTransID      = mTransID;
    SM_GET_LSN( sTempLastWritedLSN, mLastWritedLSN );

    /* Ʈ������� ȹ���� ��� lock�� �����ϰ� Ʈ����� ��Ʈ����
     * �ʱ�ȭ�� �� ��ȯ�Ѵ�. */
    IDE_TEST( end() != IDE_SUCCESS );

    /* ������ ������ Transaction�� ��� */
    /* BUG-26482 ��� �Լ��� CommitLog ��� ���ķ� �и��Ͽ� ȣ���մϴ�.
     * BUG-35452 lock ���� commit log�� standby�� �ݿ��� ���� Ȯ���ؾ��Ѵ�.*/
    /* BUG-42736 Loose Replication ���� Remote ������ �ݿ��� ��� �Ͽ��� �մϴ�. */
    if ( sNeedWaitTransForRepl == ID_TRUE )
    {
        if (smrRecoveryMgr::mIsReplCompleteAfterCommitFunc != NULL )
        {
            smrRecoveryMgr::mIsReplCompleteAfterCommitFunc(
                                                sTempStatistics,
                                                sTempTransID,
                                                SM_MAKE_SN( sTempLastWritedLSN ),
                                                ( mFlag & SMX_REPL_MASK ),
                                                SMI_AFTER_LOCK_RELEASE);
        }
    }
    else
    {
        /* do nothing */
    }
    
    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    switch( sState )
    {
        case 1:
            IDE_ASSERT( iduMemMgr::free( sOIDList ) == IDE_SUCCESS );
            sOIDList = NULL;
        default:
            break;
    }

    /* Commit Log�� ���� �Ŀ��� ����ó���ϸ� �ȵȴ�. */
    IDE_ASSERT( sWriteCommitLog == ID_FALSE );

    return IDE_FAILURE;
}

/*
    TASK-2401 MMAP Loggingȯ�濡�� Disk/Memory Log�� �и�

    Disk �� ���� Transaction�� Memory Table�� ������ ���
    Disk �� Log�� Flush

    aLSN - [IN] Sync�� ������ �Ұ��ΰ�?
*/
IDE_RC smxTrans::flushLog( smLSN *aLSN, idBool aIsCommit )
{
    /* BUG-21626 : COMMIT WRITE WAIT MODE�� �������ϰ� ���� �ʽ��ϴ�. */
    if ( ( ( mFlag & SMX_COMMIT_WRITE_MASK ) == SMX_COMMIT_WRITE_WAIT ) &&
         ( aIsCommit == ID_TRUE ) && 
         ( !( SM_IS_LSN_MAX(*aLSN) ) ) )
    {
        IDE_TEST( smrLogMgr::syncLFThread( SMR_LOG_SYNC_BY_TRX,
                                           aLSN )
                  != IDE_SUCCESS );
    }

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*****************************************************************
 *
 * BUG-22576
 *  transaction�� partial rollback�� �� DB�κ��� �Ҵ���
 *  ���������� �ִٸ� �̵��� table�� �̸� �Ŵ޾ƾ� �Ѵ�.
 *  �ֳ��ϸ� partial rollback�� �� ��ü�� ���� lock�� Ǯ ��찡
 *  �ִµ�, �׷��� �� ��ü�� ��� ���� �𸣱� ������
 *  �̸� page���� table�� �Ŵ޾Ƴ��� �Ѵ�.
 *
 *  �� �Լ��� partial abort�� lock�� Ǯ�� ����
 *  �ݵ�� �ҷ��� �Ѵ�.
 *****************************************************************/
IDE_RC smxTrans::addPrivatePageListToTableOnPartialAbort()
{
    // private page list�� table�� �ޱ� ����
    // �ݵ�� log�� sync�ؾ� �Ѵ�.
    IDE_TEST( flushLog(&mLstUndoNxtLSN, ID_FALSE /*not commit*/)
              != IDE_SUCCESS );

    IDE_TEST( finAndInitPrivatePageList() != IDE_SUCCESS );

    IDE_TEST( finAndInitVolPrivatePageList() != IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

IDE_RC smxTrans::abort()
{
    smLSN      sAbortEndLSN = {0,0};
    smSCN      sDummySCN;
    smSCN      sSysSCN;
    idBool     sNeedWaitTransForRepl = ID_FALSE;
    idvSQL   * sTempStatistics       = NULL;
    smTID      sTempTransID          = SM_NULL_TID;
    smLSN      sTempLastWritedLSN;
    
    IDE_TEST_RAISE( mAbleToRollback == ID_FALSE,
                    err_no_log );

    mStatus4FT = SMX_TX_ABORT;

    // PROJ-2068 Direct-Path INSERT�� �����Ͽ��� ��� abort �۾��� ������ �ش�.
    if ( mDPathEntry != NULL )
    {
        IDE_TEST( sdcDPathInsertMgr::abort( mDPathEntry )
                  != IDE_SUCCESS );
    }

    /* Transaction Undo��Ű�� Abort log�� ���. */
    IDE_TEST( writeAbortLogAndUndoTrans( &sAbortEndLSN )
              != IDE_SUCCESS );

    // Tx's PrivatePageList�� �����Ѵ�.
    // �ݵ�� ������ ���� abort�α׸� write�ϰ� �۾��Ѵ�.
    // BUGBUG : �α� write�Ҷ� flush�� �ݵ�� �ʿ��ϴ�.
    IDE_TEST( finAndInitPrivatePageList() != IDE_SUCCESS );

    /* PROJ-1594 Volatile TBS */
    /* Volatile TBS�� private page list�� �����Ѵ�. */
    IDE_TEST( finAndInitVolPrivatePageList() != IDE_SUCCESS );

    // BUG-14093 Ager Tx�� FreeSlot�� �͵��� ���� FreeSlotList�� �Ŵܴ�.
    IDE_TEST( smLayerCallback::processFreeSlotPending(
                                             this,
                                             &mOIDFreeSlotList )
              != IDE_SUCCESS );

    if ( ( mIsUpdate == ID_TRUE ) &&
         ( mIsTransWaitRepl == ID_TRUE ) && 
         ( isReplTrans() == ID_FALSE ) )
    {
        if (smrRecoveryMgr::mIsReplCompleteAfterCommitFunc != NULL )
        {
            smrRecoveryMgr::mIsReplCompleteAfterCommitFunc(
                                            mStatistics,
                                            mTransID,
                                            SM_MAKE_SN(mLastWritedLSN),
                                            ( mFlag & SMX_REPL_MASK ),
                                            SMI_BEFORE_LOCK_RELEASE);
        }
        else
        {
            /* Nothing to do */
        }

        sNeedWaitTransForRepl = ID_TRUE;
    }
    else
    {
        /* Nothing to do */
    }

    /* ovl�� �ִ� OID ����Ʈ�� lock�� SCN�� �����ϰ�
     * pol�� ovl�� logical ager���� �ѱ��. */
    if ( ( mTXSegEntry != NULL ) ||
         ( mOIDList->isEmpty() == ID_FALSE ) )
    {
        SM_INIT_SCN( &sDummySCN );
        IDE_TEST( addOIDList2AgingList( SM_OID_ACT_AGING_ROLLBACK,
                                        SMX_TX_ABORT,
                                        &sAbortEndLSN,
                                        &sDummySCN,
                                        ID_FALSE /* aIsLegacyTrans */ )
                  != IDE_SUCCESS );
    }
    else
    {
        mStatus = SMX_TX_ABORT;
    }

    if ( mTXSegEntry != NULL )
    {
        if (smrRecoveryMgr::isRestart() == ID_FALSE )
        {
            /*
             * [BUG-27542] [SD] TSS Page Allocation ���� �Լ�(smxTrans::allocTXSegEntry,
             *             sdcTSSegment::bindTSS)���� Exceptionó���� �߸��Ǿ����ϴ�.
             */
            if ( mTXSegEntry->mTSSlotSID != SD_NULL_SID )
            {
                /* BUG-29918 - tx�� abort�� ����ߴ� undo extent dir��
                 *             �߸��� SCN�� �Ἥ ������� ���� ext dir��
                 *             �����ϰ� �ֽ��ϴ�.
                 *
                 * markSCN()�� INITSCN�� �Ѱ��ִ� ���� �ƴ϶� GSCN�� �Ѱ��ֵ���
                 * �����Ѵ�. */
                smxTransMgr::mGetSmmViewSCN( &sSysSCN );

                IDE_TEST( sdcTXSegMgr::markSCN(
                                              mStatistics,
                                              mTXSegEntry,
                                              &sSysSCN ) != IDE_SUCCESS );


                /* BUG-31055 Can not reuse undo pages immediately after 
                 * it is used to aborted transaction 
                 * ��� ��Ȱ�� �� �� �ֵ���, ED���� Shrink�Ѵ�. */
                IDE_TEST( sdcTXSegMgr::shrinkExts( mStatistics,
                                                   this,
                                                   mTXSegEntry )
                          != IDE_SUCCESS );
            }
        }

        sdcTXSegMgr::freeEntry( mTXSegEntry,
                                ID_TRUE /* aMoveToFirst */ );
        mTXSegEntry = NULL;
    }

    /* ================================================================
     * [3] ��ũ �����ڸ� ���� pending operation���� ����
     * ================================================================ */
    IDE_TEST( executePendingList( ID_FALSE ) != IDE_SUCCESS );

    sTempStatistics    = mStatistics;
    sTempTransID       = mTransID;
    SM_GET_LSN( sTempLastWritedLSN, mLastWritedLSN );

    IDE_TEST( end() != IDE_SUCCESS );

    /* 
     * BUG-42736 Loose Replication ���� Remote ������ �ݿ��� ��� �Ͽ��� �մϴ�.
     */
    if ( sNeedWaitTransForRepl == ID_TRUE )
    {
        if (smrRecoveryMgr::mIsReplCompleteAfterCommitFunc != NULL )
        {
            smrRecoveryMgr::mIsReplCompleteAfterCommitFunc(
                                                sTempStatistics,
                                                sTempTransID,
                                                SM_MAKE_SN( sTempLastWritedLSN ),
                                                ( mFlag & SMX_REPL_MASK ),
                                                SMI_AFTER_LOCK_RELEASE);
        }
    }
    else
    {
        /* do nothing */
    }

    return IDE_SUCCESS;

    IDE_EXCEPTION(err_no_log);
    {
        IDE_SET(ideSetErrorCode( smERR_FATAL_DISABLED_ABORT_IN_LOGGING_LEVEL_0 ));
    }
    IDE_EXCEPTION_END;

    return IDE_FAILURE;

}

IDE_RC smxTrans::end()
{

    UInt  sState = 0;
    smTID sNxtTransID;
    smTID sOldTransID;

    //fix BUG-23656 session,xid ,transaction�� ������ performance view�� �����ϰ�,
    //�׵鰣�� ���踦 ��Ȯ�� �����ؾ� ��.
    // transaction end�� session id�� null�� ����.
    mSessionID = ID_NULL_SESSION_ID;

    /* PROJ-1381 Fetch Across Commits
     * TX�� ������ �� ��� table lock�� �����ؾ� �Ѵ�.
     * ������ Legacy Trans�� �����ִٸ� IS Lock�� �����ؾ� �Ѵ�. */
    if ( mLegacyTransCnt == 0 )
    {
        IDE_TEST( smLayerCallback::freeAllItemLock( mSlotN )
                  != IDE_SUCCESS );
    }
    else
    {
        IDE_TEST( smLayerCallback::freeAllItemLockExceptIS( mSlotN )
                  != IDE_SUCCESS );
    }

    //Free All Record Lock
    IDE_TEST( lock() != IDE_SUCCESS );
    sState = 1;

    (void)smLayerCallback::freeAllRecordLock( mSlotN );

    // PROJ-1362 lob curor hash, list���� ��带 �����Ѵ�.
    IDE_TEST( closeAllLobCursors() != IDE_SUCCESS );

    // PROJ-2068 Direct-Path INSERT�� ������ ���� ������ ���� ��ü�� �ı��Ѵ�.
    if ( mDPathEntry != NULL )
    {
        IDE_TEST( sdcDPathInsertMgr::destDPathEntry( mDPathEntry )
                  != IDE_SUCCESS );
        mDPathEntry = NULL;
    }

    if ( mLegacyTransCnt == 0 )
    {
        //Update Transaction ID & change commit status
        mStatus    = SMX_TX_END;
        mStatus4FT = SMX_TX_END;
    }
    else
    {
        /* nothing to do */
    }
    IDL_MEM_BARRIER;

    sOldTransID = mTransID;
    sNxtTransID = mTransID;

    do
    {
        sNxtTransID = sNxtTransID + smxTransMgr::mTransCnt;

        /* PROJ-1381 Fetch Across Commits
         * smxTrans�� Legacy Trans�� ������ smxTrans�� Legacy Trans�� TransID��
         * ���� �� �����Ƿ�, �ش� TransID�� Legacy Trans�� ���� �ʵ��� �Ѵ�. */
        if ( mLegacyTransCnt != 0 )
        {
            /* TransID ���� �ֱ�� TX �� ���� ��밡���� Statement ������
             * �����ϸ� ���� TransID�� ����� ���� ����. */
            if ( sOldTransID == sNxtTransID )
            {
                ideLog::log( IDE_SM_0,
                             "INVALID TID : OLD TID : [%u], "
                             "NEW TID : [%u]\n",
                             sOldTransID, sNxtTransID );
                IDE_ASSERT( 0 );
            }
            else
            {
                /* nothing to do */
            }

            if ( !SM_SCN_IS_MAX( smxLegacyTransMgr::getCommitSCN( sNxtTransID ) ) )
            {
                continue;
            }
            else
            {
                /* nothing to do */
            }
        }
        else
        {
            /* nothing to do */
        }
    } while ( ( sNxtTransID == 0 ) || ( sNxtTransID == SM_NULL_TID ) );

    mTransID = sNxtTransID;

#ifdef PROJ_2181_DBG
    ULong sNext;
    sNext = mTransIDDBG + smxTransMgr::mTransCnt;

    while ( (sNext == 0LL) || (sNext == ID_ULONG_MAX) )
    {
        sNext = sNext+ smxTransMgr::mTransCnt;
    }

    mTransIDDBG = sNext;
#endif

    IDL_MEM_BARRIER;

    IDE_TEST( init() != IDE_SUCCESS ); //checkpoint thread ����..

    sState = 0;
    IDE_TEST( unlock() != IDE_SUCCESS );

    //Savepoint Resource�� ��ȯ�Ѵ�.
    IDE_TEST( mSvpMgr.destroy() != IDE_SUCCESS );
    IDE_TEST( removeAllAndReleaseMutex() != IDE_SUCCESS );

    mStatistics = NULL;

    if ( mCompRes != NULL )
    {
        // Log ���� ���ҽ��� Pool�� �ݳ��Ѵ�.
        IDE_TEST( mCompResPool.freeCompRes( mCompRes )
                  != IDE_SUCCESS );
        mCompRes = NULL;
    }
    else
    {
        // Transaction�� �α��� �ѹ��� ���� ���� ���
        // ���� ���ҽ��� Pool�κ��� ���������� ����.
        // Do Nothing!
    }

    /* TASK-2401 MMAP Loggingȯ�濡�� Disk/Memory Log�� �и�
       Disk/Memory Table�� �����ߴ��� ���θ� �ʱ�ȭ
     */
    mDiskTBSAccessed = ID_FALSE;
    mMemoryTBSAccessed = ID_FALSE;
    mMetaTableModified = ID_FALSE;

    // Ʈ������� ����� Touch Page List�� �����Ѵ�.
    IDE_ASSERT( mTouchPageList.reset() == IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    if ( sState != 0 )
    {
        IDE_PUSH();
        IDE_ASSERT( unlock() == IDE_SUCCESS );
        IDE_POP();
    }

    return IDE_FAILURE;

}

IDE_RC smxTrans::setLstUndoNxtLSN( smLSN aLSN )
{
    IDE_TEST( lock() != IDE_SUCCESS );

    if ( mIsUpdate == ID_FALSE )
    {
        /* Tx�� ù �α׸� ����Ҷ��� mIsUpdate == ID_FALSE */
        mIsUpdate = ID_TRUE;

        /* �Ϲ����� Tx�� ���, mFstUndoNxtLSN�� SM_LSN_MAX �̴�. */
        if ( SM_IS_LSN_MAX( mFstUndoNxtLSN ) )
        {
            SM_GET_LSN( mFstUndoNxtLSN, aLSN );
        }
        else
        {
            /* BUG-39404
             * Legacy Tx�� ���� Tx�� ���, mFstUndoNxtLSN��
             * ���� Legacy Tx�� mFstUndoNxtLSN �� �����ؾ� �� */
        }

        mFstUpdateTime = smLayerCallback::smiGetCurrentTime();
    
    }
    else
    {
        /* nothing to do */
    }

    /* mBeginLogLSN�� recovery from replication���� ����ϸ� 
     * begin���� �ʱ�ȭ �ϸ�, 
     * Active Transaction List �� ��ϵɶ� ���� �����Ѵ�.
     * FAC ������ mFstUndoNxtLSN <= mBeginLogLSN ��.
     */
    if ( SM_IS_LSN_MAX( mBeginLogLSN ) )
    {
        SM_GET_LSN( mBeginLogLSN, aLSN ); 
    }
    else
    {
        /* nothing to do */
    }

    /* BUG-33895 [sm_recovery] add the estimate function
     * the time of transaction undoing. */
    mTotalLogCount ++;
    mLstUndoNxtLSN = aLSN;
    SM_GET_LSN( mLastWritedLSN, aLSN );
    
    IDE_TEST( unlock() != IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*******************************************************************
 * BUG-16368�� ���� ������ �Լ�
 * Description:
 *              smxTrans::addListToAgerAtCommit �Ǵ�
 *              smxTrans::addListToAgerAtABort ���� �Ҹ���� �Լ�.
 * aLSN             [IN]    - �����Ǵ� OidList�� SCN�� ����
 * aAgingState      [IN]    - SMX_TX_COMMT OR SMX_TX_ABORT
 * aAgerNormalList  [OUT]   - List�� add�� ��� ����
 *******************************************************************/
IDE_RC smxTrans::addOIDList2LogicalAger( smLSN        * aLSN,
                                         SInt           aAgingState,
                                         void        ** aAgerNormalList )
{
    // PRJ-1476
    // cached insert oid�� memory ager���� �Ѿ�� �ʵ��� ��ġ�� ���Ѵ�.
    IDE_TEST( mOIDList->cloneInsertCachedOID() != IDE_SUCCESS );

    mOIDList->mOIDNodeListHead.mPrvNode->mNxtNode = NULL;
    mOIDList->mOIDNodeListHead.mNxtNode->mPrvNode = NULL;

    IDE_TEST( smLayerCallback::addList2LogicalAger( mTransID,
                                                    mIsDDL,
                                                    &mCommitSCN,
                                                    aLSN,
                                                    aAgingState,
                                                    mOIDList,
                                                    aAgerNormalList )
              != IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;
    return IDE_FAILURE;
}


/*
 * Transaction�� Commit�α׸� ����Ѵ�.
 *
 * aEndLSN - [OUT] ��ϵ� Commit Log�� LSN
 *
 */
IDE_RC smxTrans::writeCommitLog( smLSN* aEndLSN )
{
    /* =======================
     * [1] write precommit log
     * ======================= */
    /* ------------------------------------------------
     * 1) tss�� ���þ��� tx��� commit �α��� �ϰ� addListToAger�� ó���Ѵ�.
     * commit �α��Ŀ� tx ����(commit ����)�� �����Ѵ�.
     * -> commit �α� -> commit ���·� ���� -> commit SCN ���� -> tx end
     *
     * 2) tss�� ���õ� tx(disk tx)�� addListToAger���� commit SCN�� �Ҵ�����
     * tss ���� �۾��� ���Ŀ� commit �α��� �� ����, commit SCN�� �����Ѵ�.
     * add TSS commit list -> commit �α� -> commit���·κ���->commit SCN ����
     * tss�� commitSCN���� -> tx end
     * ----------------------------------------------*/
    if ( mTXSegEntry == NULL  )
    {
        IDE_TEST( writeCommitLog4Memory( aEndLSN ) != IDE_SUCCESS );
    }
    else
    {
        IDE_TEST( writeCommitLog4Disk( aEndLSN ) != IDE_SUCCESS );

        IDU_FIT_POINT( "1.PROJ-1548@smxTrans::writeCommitLog" );
    }

    /* BUG-19503: DRDB�� smcTableHeader�� mFixed:.Mutex�� Duration Time�� ��ϴ�.
     *
     *            RecordCount ������ ���ؼ� Mutex Lock�� ���� ���¿��� RecordCount
     *            ������ �����ϰ� �ֽ��ϴ�. �� �αװ� Commit�α��ε� ������
     *            Transaction�� Durability�� �����Ѵٸ� Commit�αװ� ��ũ�� sync
     *            �ɶ����� ��ٷ��� �ϰ� Ÿ Transaction���� �̸� ��ٸ��� ������
     *            �ֽ��ϴ�. �Ͽ� Commit�α� ��Ͻÿ� Flush�ϴ� ���� �ƴ϶� ���Ŀ�
     *            Commit�αװ� Flush�� �ʿ������� Flush�ϴ°����� �����߽��ϴ�.*/

    /* Durability level�� ���� commit �α״� sync�� �����ϱ⵵ �Ѵ�.
     * �Լ��ȿ��� Flush�� �ʿ��ϴ����� Check�մϴ�. */

    // log�� disk�� ��ϵɶ����� ��ٸ����� ���� ���� ȹ��
    if ( ( hasPendingOp() == ID_TRUE ) ||
         ( isCommitWriteWait() == ID_TRUE ) )
    {
        IDE_TEST( smrLogMgr::syncLFThread( SMR_LOG_SYNC_BY_TRX,
                                           aEndLSN )
                  != IDE_SUCCESS );
    }
    else
    {
        /* nothing to do ... */
    }

    return IDE_SUCCESS;
    IDE_EXCEPTION_END;
    return IDE_FAILURE;
}

/*
 * ���� Tranasction�� ���ſ����� ������ Transaction�̶��
 *   1. Abort Prepare Log�� ����Ѵ�.
 *   2. Undo�� �����Ѵ�.
 *   3. Abort Log�� ����Ѵ�.
 *   4. Volatile Table�� ���� ���� ������ �־��ٸ� Volatile��
 *      ���ؼ� Undo�۾��� �����Ѵ�.
 *
 * aEndLSN - [OUT] Abort Log�� End LSN.
 *
 */
IDE_RC smxTrans::writeAbortLogAndUndoTrans( smLSN* aEndLSN )
{
    // PROJ-1553 Replication self-deadlock
    smrTransPreAbortLog  sTransPreAbortLog;
    smrTransAbortLog     sAbortLog;
    smrTransAbortLog   * sAbortLogHead;
    smLSN                sLSN;
    smSCN                sDummySCN;
    sdrMtx               sMtx;
    smuDynArrayBase    * sDynArrayPtr;
    UInt                 sDskRedoSize;
    smLSN                sEndLSNofAbortLog;
    sdcTSSegment       * sCTSegPtr;
    idBool               sIsDiskTrans = ID_FALSE;

    /* PROJ-1594 Volatile TBS */
    /* Volatile TBS�� ���� ���� ������ ��� undo�Ѵ�. */
    if ( svrLogMgr::isOnceUpdated( &mVolatileLogEnv ) == ID_TRUE )
    {
        IDE_TEST( svrRecoveryMgr::undoTrans( &mVolatileLogEnv,
                                             SVR_LSN_BEFORE_FIRST )
                 != IDE_SUCCESS );
    }
    else
    {
        /* nothing to do */
    }

    if ( mIsUpdate == ID_TRUE )
    {
        // PROJ-1553 Replication self-deadlock
        // undo �۾��� �����ϱ� ���� pre-abort log�� ��´�.
        // �ܺο��� ���� undoTrans()�� ȣ���ϴ� �Ϳ� ���ؼ���
        // �Ű澲�� �ʾƵ� �ȴ�.
        // �ֳ��ϸ� SM ��ü������ undo�� �����ϴ� �Ϳ� ���ؼ���
        // replication�� receiver�ʿ��� self-deadlock�� ������
        // �ȵǱ� �����̴�.
        initPreAbortLog( &sTransPreAbortLog );

        // write pre-abort log
        IDE_TEST( smrLogMgr::writeLog( NULL, /* idvSQL* */
                                       this,
                                       (SChar*)&sTransPreAbortLog,
                                       NULL,  // Previous LSN Ptr
                                       NULL,  // Log LSN Ptr
                                       &sEndLSNofAbortLog ) // End LSN Ptr
                  != IDE_SUCCESS );

        // Hybrid Tx : �ڽ��� ����� Log�� Flush
        IDE_TEST( flushLog( &sEndLSNofAbortLog,
                            ID_FALSE /* When Abort */ ) != IDE_SUCCESS );

        // pre-abort log�� ���� �� undo �۾��� �����Ѵ�.
        SM_LSN_MAX( sLSN );
        IDE_TEST( smrRecoveryMgr::undoTrans( mStatistics,
                                             this,
                                             &sLSN) != IDE_SUCCESS );

        /* ------------------------------------------------
         * 1) tss�� ���þ��� tx��� abort �α��� �ϰ� addListToAger�� ó���Ѵ�.
         * abort �α��Ŀ� tx ����(in-memory abort ����)�� �����Ѵ�.
         * -> abort �α� -> tx in-memory abort ���·� ���� -> tx end
         *
         * 2) ������ tss�� ���õ� tx�� addListToAger���� freeTSS
         * tss ���� �۾��� ���Ŀ� abort �α��� �Ѵ�.
         * -> tx in-memory abort ���·� ���� -> abort �α� -> tx end
         * ----------------------------------------------*/
        // BUG-29262 TSS �Ҵ翡 ������ Ʈ������� COMMIT �α׸� ����ؾ� �մϴ�.
        // log buffer �ʱ�ȭ
        initLogBuffer();

        // BUG-27542 : (mTXSegEntry != NULL) &&
        // (mTXSegEntry->mTSSlotSID != SD_NULL_SID) �̶�� Disk Tx�̴�
        if ( mTXSegEntry != NULL )
        {
            if ( mTXSegEntry->mTSSlotSID != SD_NULL_SID )
            {
                sIsDiskTrans = ID_TRUE;
            }
        }

        if ( sIsDiskTrans == ID_TRUE )
        {
            initAbortLog( &sAbortLog, SMR_LT_DSKTRANS_ABORT );

            // BUG-29262 TSS �Ҵ翡 ������ Ʈ������� COMMIT �α׸� ����ؾ� �մϴ�.
            // abort �α� ���
            IDE_TEST( writeLogToBuffer(
                          &sAbortLog,
                          SMR_LOGREC_SIZE(smrTransAbortLog) ) != IDE_SUCCESS );

            // BUG-31504: During the cached row's rollback, it can be read.
            // abort ���� row�� �ٸ� Ʈ����ǿ� ���� �������� �ȵȴ�.
            SM_SET_SCN_INFINITE( &sDummySCN );


            IDE_TEST( sdrMiniTrans::begin( mStatistics,
                                           &sMtx,
                                           this,
                                           SDR_MTX_LOGGING,
                                           ID_FALSE,/*MtxUndoable(PROJ-2162)*/
                                           SM_DLOG_ATTR_DEFAULT |
                                           SM_DLOG_ATTR_TRANS_LOGBUFF )
                      != IDE_SUCCESS );

            /*
             * Ʈ����� Abort�ÿ��� CommitSCN�� �ý������κ��� ���� �ʿ䰡
             * ���� ������ Abort�α׷� Unbind TSS�� �ٷ� �����Ѵ�.
             */
            sCTSegPtr = sdcTXSegMgr::getTSSegPtr( mTXSegEntry );

            IDE_TEST( sCTSegPtr->unbindTSS4Abort(
                                          mStatistics,
                                          &sMtx,
                                          mTXSegEntry->mTSSlotSID,
                                          &sDummySCN ) != IDE_SUCCESS );

            sDynArrayPtr = &(sMtx.mLogBuffer);
            sDskRedoSize = smuDynArray::getSize( sDynArrayPtr );

            IDE_TEST( writeLogToBufferUsingDynArr(
                                          sDynArrayPtr,
                                          sDskRedoSize ) != IDE_SUCCESS );

            // BUG-29262 TSS �Ҵ翡 ������ Ʈ������� COMMIT �α׸� ����ؾ� �մϴ�.
            // abort �α��� tail ���
            IDE_TEST( writeLogToBuffer( &(sAbortLog.mHead.mType),
                                        ID_SIZEOF( smrLogType ) )
                      != IDE_SUCCESS );

            // BUG-29262 TSS �Ҵ翡 ������ Ʈ������� COMMIT �α׸� ����ؾ� �մϴ�.
            // abort �α��� logHead�� disk redo log size�� ���
            sAbortLogHead = (smrTransAbortLog*)mLogBuffer;

            smrLogHeadI::setSize( &sAbortLogHead->mHead, mLogOffset );
            sAbortLogHead->mDskRedoSize = sDskRedoSize;

            IDE_TEST( sdrMiniTrans::commit( &sMtx,
                                            SMR_CT_END,
                                            aEndLSN,
                                            SMR_RT_DISKONLY,
                                            NULL )  /* aBeginLSN */
                      != IDE_SUCCESS );
        }
        else
        {
            initAbortLog( &sAbortLog, SMR_LT_MEMTRANS_ABORT );

            // BUG-29262 TSS �Ҵ翡 ������ Ʈ������� COMMIT �α׸� ����ؾ� �մϴ�.
            // abort �α� ���
            IDE_TEST( writeLogToBuffer(
                          &sAbortLog,
                          SMR_LOGREC_SIZE(smrTransAbortLog) ) != IDE_SUCCESS );

            // BUG-29262 TSS �Ҵ翡 ������ Ʈ������� COMMIT �α׸� ����ؾ� �մϴ�.
            // abort �α��� tail ���
            IDE_TEST( writeLogToBuffer( &(sAbortLog.mHead.mType),
                                        ID_SIZEOF(smrLogType) ) 
                      != IDE_SUCCESS );

            IDE_TEST( smrLogMgr::writeLog( mStatistics, /* idvSQL* */
                                           this,
                                           (SChar*)mLogBuffer,
                                           NULL,  // Previous LSN Ptr
                                           NULL,  // Log LSN Ptr
                                           aEndLSN ) // End LSN Ptr
                      != IDE_SUCCESS );
        }

        /* LogFile�� sync�ؾ� �Ǵ��� ���θ� Ȯ�� �� �ʿ��ϴٸ� sync �Ѵ�. */
        if ( hasPendingOp() == ID_TRUE )
        {
            IDE_TEST( smrLogMgr::syncLFThread( SMR_LOG_SYNC_BY_TRX,
                                               aEndLSN )
                      != IDE_SUCCESS );
        }
        else
        {
            /* nothing to do ... */
        }

    }

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;

}

/***********************************************************************
 * Description : MMDB(Main Memory DB)�� ���� ������ �۾��� ������ Transaction
 *               �� Commit Log�� ����Ѵ�.
 *
 * aEndLSN - [OUT] Commit Log�� EndLSN
 **********************************************************************/
IDE_RC smxTrans::writeCommitLog4Memory( smLSN* aEndLSN )
{
    smrTransCommitLog *sTransCommitLog;

    IDE_TEST( mTableInfoMgr.requestAllEntryForCheckMaxRow()
              != IDE_SUCCESS );

    IDE_TEST( mTableInfoMgr.releaseEntryAndUpdateMemTableInfoForIns()
              != IDE_SUCCESS );

    sTransCommitLog = (smrTransCommitLog*)getLogBuffer();
    initCommitLog( sTransCommitLog, SMR_LT_MEMTRANS_COMMIT );

    IDE_TEST( smrLogMgr::writeLog( NULL, /* idvSQL* */
                                   this,
                                   (SChar*)sTransCommitLog,
                                   NULL, // Previous LSN Ptr
                                   NULL, // Log Begin LSN
                                   aEndLSN ) // End LSN Ptr
              != IDE_SUCCESS );

    IDU_FIT_POINT( "1.PROJ-1548@smxTrans::writeCommitLog4Memory" );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/***********************************************************************
 * Description : DRDB(Disk Resident DB)�� ���� ������ �۾��� ������ Transaction
 *               �� Commit Log�� ����Ѵ�.
 *
 * aEndLSN - [OUT] Commit Log�� EndLSN
 **********************************************************************/
IDE_RC smxTrans::writeCommitLog4Disk( smLSN* aEndLSN )
{


    IDE_TEST( mTableInfoMgr.requestAllEntryForCheckMaxRow()
              != IDE_SUCCESS );

    IDE_TEST( mTableInfoMgr.releaseEntryAndUpdateMemTableInfoForIns()
              != IDE_SUCCESS );

    /* Table Header�� Record Count�� �������Ŀ� Commit�α׸� ����Ѵ�.*/
    IDE_TEST( mTableInfoMgr.releaseEntryAndUpdateDiskTableInfoWithCommit(
                                                      mStatistics,
                                                      this,     /* aTransPtr */
                                                      NULL,     /* aBeginLSN */
                                                      aEndLSN ) /* aEndLSN   */
              != IDE_SUCCESS );


    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*********************************************************
  function description: Aging List�� Transaction�� OID List
  �� �߰��Ѵ�.
   -  ������ ���� ������ �����ϴ� ��� Commit SCN�� �Ҵ��Ѵ�.
     1. OID List or Delete OID List�� ������� ���� ���
     2. Disk���� DML�� ������ ���

   - For MMDB
     1. If commit, �Ҵ�� Commit SCN�� Row Header, Table Header
        �� Setting�Ѵ�.

   - For DRDB
     1. Transaction�� ��ũ ���̺��� ���Ͽ� DML�� �Ѱ�쿡��
        TSS slot�� commitSCN�� setting�ϰ� tss slot list�� head
        �� �ܴ�.

        * ���� �ΰ��� Action�� �ϳ��� Mini Transaction���� ó���ϰ�
          Logging�� ���� �ʴ´�.

   - For Exception
     1. ���� ���� mini transaction �����߿� crash�Ǹ�,
        restart recovery��������,Ʈ�������  tss slot��
        �Ҵ�޾Ұ�, commit log�� ����µ�,
        TSS slot�� commitSCN�� infinite�̸� ������ ����
        redo�� �Ѵ�.
        -  TSS slot�� commitSCN��  0���� setting�ϰ�, �ش�
           TSS slot�� tss list�� �Ŵ޾Ƽ� GC�� �̷��������
           �Ѵ�.
***********************************************************/
IDE_RC smxTrans::addOIDList2AgingList( SInt       aAgingState,
                                       smxStatus  aStatus,
                                       smLSN*     aEndLSN,
                                       smSCN*     aCommitSCN,
                                       idBool     aIsLegacyTrans )
{
    // oid list�� ��� �ִ°�?
    smSCN                sDummySCN;
    smxProcessOIDListOpt sProcessOIDOpt;
    void               * sAgerNormalList = NULL;
    ULong                sAgingOIDCnt;
    UInt                 sState = 0;

    /* PROJ-1381 */
    IDE_DASSERT( ( aIsLegacyTrans == ID_TRUE  ) ||
                 ( aIsLegacyTrans == ID_FALSE ) );

    // tss commit list�� commitSCN������ ���ĵǾ� insert�ȴ�.
    // -> smxTransMgr::mMutex�� ���� �����̱⶧����.
    IDE_TEST( smxTransMgr::lock() != IDE_SUCCESS );
    sState = 1;

    if ( aStatus == SMX_TX_COMMIT )
    {
        /*  Ʈ������� oid list�Ǵ�  deleted oid list�� ��� ���� ������,
            commitSCN�� system���� ���� �Ҵ�޴´�.
            %Ʈ������� statement�� ��� disk ���� DML�̾�����
            oid list�� ������� �� �ִ�.
            --> �̶��� commitSCN�� ���� �Ѵ�. */

        //commit SCN�� �Ҵ�޴´�. �ȿ��� transaction ���� ����
        //tx�� in-memory COMMIT or ABORT ����
        IDE_TEST( smxTransMgr::mGetSmmCommitSCN( this,
                                                 aIsLegacyTrans,
                                                 (void *)&aStatus )
                 != IDE_SUCCESS );
        SM_SET_SCN(aCommitSCN,&mCommitSCN);

        /* BUG-41814
         * Fetch Cursor�� �� �޸� Stmt�� ������ ���� Ʈ������� commit ��������
         * �����ִ� �޸� Stmt�� 0�� �̸�, ���� mMinMemViewSCN �� ���Ѵ� �̴�.
         * mMinMemViewSCN�� ���Ѵ��� Ʈ������� Ager�� ��ü Ʈ������� minMemViewSCN��
         * ����Ҷ� �����ϰ� �Ǵµ�, ���� ���� �ʵ��� commitSCN�� �ӽ÷� �ھƵд�. */
        if ( SM_SCN_IS_INFINITE( ((smxTrans *)this)->mMinMemViewSCN ) )
        {
            SM_SET_SCN( &(((smxTrans *)this)->mMinMemViewSCN), aCommitSCN );
        }
        else
        {
            /* Nothing to do */
        }

        IDU_FIT_POINT( "1.BUG-21950@smxTrans::addOIDList2AgingList" );
    }
    else
    {
        /* fix BUG-9734, rollback�ÿ� system scn�� ������Ű�°���
         * ���� ���Ͽ� �׳� 0�� setting�Ѵ�. */
        SM_INIT_SCN(&sDummySCN);
        setTransCommitSCN( this, sDummySCN, &aStatus );
    }

    /* PROJ-2462 ResultCache */
    mTableInfoMgr.addTablesModifyCount();

    // To fix BUG-14126
    if ( mOIDList->isEmpty() == ID_FALSE )
    {
        if ( ( mOIDList->needAging() == ID_FALSE ) &&
             ( aStatus == SMX_TX_COMMIT ) )
        {
            /* PROJ-1381 Fetch Across Commits
             * Insert�� �����ϸ� addOIDList2LogicalAger�� ȣ������ �ʾƼ�
             * OIDList���� mCacheOIDNode4Insert�� �������� �ʴ´�.
             * FAC�� Commit ���Ŀ��� OIDList�� ����ϹǷ� �����ϵ��� �Ѵ�. */
            if ( aIsLegacyTrans == ID_TRUE )
            {
                IDE_TEST( mOIDList->cloneInsertCachedOID()
                          != IDE_SUCCESS );
            }

            /* Aging List�� OID List�� �߰����� �ʴ´ٸ� Transaction��
               ���� OID List�� Free��Ų��. */
            sProcessOIDOpt = SMX_OIDLIST_DEST;
        }
        else
        {
            IDE_TEST( addOIDList2LogicalAger( aEndLSN,
                                              aAgingState,
                                              &sAgerNormalList )
                      != IDE_SUCCESS );

            /* Aging List�� OID List�� �߰��Ѵٸ� Ager Thread��
               OID List�� Free��Ų��. */
            sProcessOIDOpt = SMX_OIDLIST_INIT;
        }

        sState = 0;
        IDE_TEST( smxTransMgr::unlock() != IDE_SUCCESS );

        IDU_FIT_POINT( "BUG-45654@smxTrans::addOIDList2AgingList::beforeProcessOIDList" );

        IDE_TEST( mOIDList->processOIDList( aAgingState,
                                            aEndLSN,
                                            mCommitSCN,
                                            sProcessOIDOpt,
                                            &sAgingOIDCnt,
                                            aIsLegacyTrans )
                  != IDE_SUCCESS );


        /* BUG-17417 V$Ager������ Add OID������ ���� Ager��
         *                     �ؾ��� �۾��� ������ �ƴϴ�.
         *
         * Aging OID������ �����ش�. */
        if ( sAgingOIDCnt != 0 )
        {
            smLayerCallback::addAgingRequestCnt( sAgingOIDCnt );
        }

        if ( sAgerNormalList != NULL )
        {
            smLayerCallback::setOIDListFinished( sAgerNormalList,
                                                 ID_TRUE );
        }
    }
    else
    {
        sState = 0;
        IDE_TEST( smxTransMgr::unlock() != IDE_SUCCESS );
    }

    // TSS���� commitSCN���� setting�Ѵ�.
    IDU_FIT_POINT( "8.PROJ-1552@smxTrans::addOIDList2AgingList" );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    if ( sState != 0 )
    {
        IDE_ASSERT( smxTransMgr::unlock() == IDE_SUCCESS );
    }

    return IDE_FAILURE;
}

IDE_RC smxTrans::removeAllAndReleaseMutex()
{
    iduMutex * sMutex;
    smuList  * sIterator;
    smuList  * sNodeNext;

    for (sIterator = SMU_LIST_GET_FIRST(&mMutexList),
         sNodeNext = SMU_LIST_GET_NEXT(sIterator)
         ;
         sIterator != &mMutexList
         ;
         sIterator = sNodeNext,
         sNodeNext = SMU_LIST_GET_NEXT(sIterator))
    {
        sMutex = (iduMutex*)sIterator->mData;
        IDE_TEST_RAISE( sMutex->unlock() != IDE_SUCCESS,
                        mutex_unlock_error);
        SMU_LIST_DELETE(sIterator);
        IDE_TEST( mMutexListNodePool.memfree((void *)sIterator) != IDE_SUCCESS );
    }

    return IDE_SUCCESS;

    IDE_EXCEPTION(mutex_unlock_error);
    {
        IDE_SET(ideSetErrorCode(smERR_FATAL_ThrMutexUnlock));
    }
    IDE_EXCEPTION_END;

    return IDE_FAILURE;

}

/* ------------------------------------------------
             For Global Transaction
    xa_commit, xa_rollback, xa_start��
    local transaction�� �������̽��� �״�� �����.
   ------------------------------------------------ */
/* BUG-18981 */
IDE_RC smxTrans::prepare(ID_XID *aXID)
{

    PDL_Time_Value    sTmVal;

    if ( mIsUpdate == ID_TRUE )
    {
        if ( mTXSegEntry != NULL )
        {
            IDE_TEST( smrUpdate::writeXaSegsLog(
                         NULL, /* idvSQL * */
                         (void*)this,
                         aXID,
                         mLogBuffer,
                         mTXSegEntryIdx,
                         mTXSegEntry->mExtRID4TSS,
                         mTXSegEntry->mTSSegmt.getFstPIDOfCurAllocExt(),
                         mTXSegEntry->mFstExtRID4UDS,
                         mTXSegEntry->mLstExtRID4UDS,
                         mTXSegEntry->mUDSegmt.getFstPIDOfCurAllocExt(),
                         mTXSegEntry->mFstUndoPID,
                         mTXSegEntry->mLstUndoPID ) != IDE_SUCCESS );
        }

        /* ---------------------------------------------------------
           table lock�� prepare log�� ����Ÿ�� �α�
           record lock�� OID ������ ����� ȸ���� ����� �ܰ迡�� �����ؾ� ��
           ---------------------------------------------------------*/
        IDE_TEST( smLayerCallback::logLock( (void*)this,
                                            mTransID,
                                            mLogTypeFlag,
                                            aXID,
                                            mLogBuffer,
                                            mSlotN,
                                            &mFstDskViewSCN )
                  != IDE_SUCCESS );

    }

    /* ----------------------------------------------------------
       Ʈ����� commit ���� ���� �� Gloabl Transaction ID setting
       ---------------------------------------------------------- */
    sTmVal = idlOS::gettimeofday();

    IDE_TEST( lock() != IDE_SUCCESS );
    mCommitState = SMX_XA_PREPARED;
    mPreparedTime = (timeval)sTmVal;
    mXaTransID =  *aXID;

    IDE_TEST( unlock() != IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;
    return IDE_FAILURE;

}

/*
IDE_RC smxTrans::forget(XID *aXID, idBool a_isRecovery)
{

    smrXaForgetLog s_forgetLog;

    if (mIsUpdate == ID_TRUE && a_isRecovery != ID_TRUE )
    {
        s_forgetLog.mHead.mTransID   = mTransID;
        s_forgetLog.mHead.mType      = SMR_LT_XA_FORGET;
        s_forgetLog.mHead.mSize     = SMR_LOGREC_SIZE(smrXaForgetLog);
        s_forgetLog.mHead.mFlag     = mLogTypeFlag;
        s_forgetLog.mXaTransID        = mXaTransID;
        s_forgetLog.mTail             = SMR_LT_XA_FORGET;

        IDE_TEST( smrLogMgr::writeLog(this, (SChar*)&s_forgetLog) != IDE_SUCCESS );
    }

    mTransID   = mTransID + smxTransMgr::mTransCnt;

    initXID();

    IDE_TEST( init() != IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}
*/

IDE_RC smxTrans::freeOIDList()
{
    if ( mOIDToVerify != NULL )
    {
        IDE_TEST( mOIDToVerify->freeOIDList() != IDE_SUCCESS ); 
    }

    IDE_TEST( mOIDList->freeOIDList() != IDE_SUCCESS );
    IDE_TEST( mOIDFreeSlotList.freeOIDList() != IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

void smxTrans::showOIDList()
{

    mOIDList->dump();
    mOIDFreeSlotList.dump();

}

/* BUG-18981 */
IDE_RC smxTrans::getXID( ID_XID *aXID )
{
    idBool sIsValidXID;

    sIsValidXID = isValidXID();
    IDE_TEST( sIsValidXID != ID_TRUE );
    *aXID = mXaTransID;

    return IDE_SUCCESS;
    IDE_EXCEPTION_END;
    return IDE_FAILURE;
}

void smxTrans::initLogBuffer()
{
    mLogOffset = 0;
}

IDE_RC smxTrans::writeLogToBufferUsingDynArr(smuDynArrayBase* aLogBuffer,
                                             UInt             aLogSize)
{
    IDE_TEST( writeLogToBufferUsingDynArr( aLogBuffer,
                                           mLogOffset,
                                           aLogSize ) != IDE_SUCCESS );
    mLogOffset += aLogSize;

    return IDE_SUCCESS;
    IDE_EXCEPTION_END;
    return IDE_FAILURE;
}

IDE_RC smxTrans::writeLogToBufferUsingDynArr(smuDynArrayBase* aLogBuffer,
                                             UInt             aOffset,
                                             UInt             aLogSize)
{
    SChar * sLogBuffer;
    UInt    sState  = 0;

    if ( (aOffset + aLogSize) >= mLogBufferSize )
    {
        mLogBufferSize = idlOS::align( mLogOffset+aLogSize,
                                       SMX_TRANSACTION_LOG_BUFFER_ALIGN_SIZE );

        sLogBuffer = NULL;

        /* smxTrans_writeLogToBufferUsingDynArr_malloc_LogBuffer.tc */
        IDU_FIT_POINT("smxTrans::writeLogToBufferUsingDynArr::malloc::LogBuffer");
        IDE_TEST( iduMemMgr::malloc( IDU_MEM_SM_TRANSACTION_TABLE,
                                     mLogBufferSize,
                                     (void**)&sLogBuffer,
                                     IDU_MEM_FORCE )
                 != IDE_SUCCESS );
        sState = 1;

        if ( aOffset != 0 )
        {
            idlOS::memcpy(sLogBuffer, mLogBuffer, aOffset);
        }

        IDE_TEST( iduMemMgr::free(mLogBuffer) != IDE_SUCCESS );

        mLogBuffer = sLogBuffer;

        // ���� �α׹��۸� �̿��Ͽ�  ����Ǵ� �α��� ũ���� ������
        // �̹� �������ֱ� ������ ����α� ������ ũ��� ������ �ʿ䰡 ����.
    }

    smuDynArray::load( aLogBuffer, 
                       (mLogBuffer + aOffset),
                       mLogBufferSize - aOffset );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    switch( sState )
    {
        case 1:
            IDE_ASSERT( iduMemMgr::free( sLogBuffer ) == IDE_SUCCESS );
            sLogBuffer = NULL;
        default:
            break;
    }

    return IDE_FAILURE;
}

IDE_RC smxTrans::writeLogToBuffer(const void *aLog, UInt aLogSize)
{
    IDE_TEST( writeLogToBuffer( aLog, mLogOffset, aLogSize ) != IDE_SUCCESS );
    mLogOffset += aLogSize;

    return IDE_SUCCESS;
    IDE_EXCEPTION_END;
    return IDE_FAILURE;
}

IDE_RC smxTrans::writeLogToBuffer(const void *aLog,
                                  UInt        aOffset,
                                  UInt        aLogSize)
{
    SChar * sLogBuffer;
    UInt    sBfrBuffSize;
    UInt    sState  = 0;

    if ( (aOffset + aLogSize) >= mLogBufferSize)
    {
        sBfrBuffSize   = mLogBufferSize;
        mLogBufferSize = idlOS::align( aOffset + aLogSize,
                                       SMX_TRANSACTION_LOG_BUFFER_ALIGN_SIZE);

        sLogBuffer = NULL;

        /* smxTrans_writeLogToBuffer_malloc_LogBuffer.tc */
        IDU_FIT_POINT("smxTrans::writeLogToBuffer::malloc::LogBuffer");
        IDE_TEST( iduMemMgr::malloc( IDU_MEM_SM_TRANSACTION_TABLE,
                                     mLogBufferSize,
                                     (void**)&sLogBuffer,
                                     IDU_MEM_FORCE )
                 != IDE_SUCCESS );
        sState = 1;

        if ( sBfrBuffSize != 0 )
        {
            idlOS::memcpy( sLogBuffer, mLogBuffer, sBfrBuffSize );
        }
        else
        {
            /* nothing to do */
        }

        IDE_TEST( iduMemMgr::free(mLogBuffer) != IDE_SUCCESS );

        mLogBuffer = sLogBuffer;
    }
    else
    {
        /* nothing to do */
    }

    idlOS::memcpy(mLogBuffer + aOffset, aLog, aLogSize);

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    switch( sState )
    {
        case 1:
            IDE_ASSERT( iduMemMgr::free( sLogBuffer ) == IDE_SUCCESS );
            sLogBuffer = NULL;
        default:
            break;
    }

    return IDE_FAILURE;
}

void smxTrans::getTransInfo(void*   aTrans,
                            SChar** aTransLogBuffer,
                            smTID*  aTID,
                            UInt*   aTransLogType)
{
    smxTrans* sTrans = (smxTrans*) aTrans;

    *aTransLogBuffer = sTrans->getLogBuffer();
    *aTID =  sTrans->mTransID;
    *aTransLogType = sTrans->mLogTypeFlag;
}

smLSN smxTrans::getTransLstUndoNxtLSN(void* aTrans)
{
    return ((smxTrans*)aTrans)->mLstUndoNxtLSN;
}

/* Transaction�� ���� UndoNxtLSN�� return */
smLSN smxTrans::getTransCurUndoNxtLSN(void* aTrans)
{
    return ((smxTrans*)aTrans)->mCurUndoNxtLSN;
}

/* Transaction�� ���� UndoNxtLSN�� Set */
void smxTrans::setTransCurUndoNxtLSN(void* aTrans, smLSN *aLSN)
{
    /* BUG-33895 [sm_recovery] add the estimate function 
     * the time of transaction undoing. */
    if ( ((smxTrans*)aTrans)->mProcessedUndoLogCount == 0 )
    {
        /* ���� ���� */
        ((smxTrans*)aTrans)->mUndoBeginTime = 
            smLayerCallback::smiGetCurrentTime();
    }
    ((smxTrans*)aTrans)->mProcessedUndoLogCount ++;
    ((smxTrans*)aTrans)->mCurUndoNxtLSN = *aLSN;
}

smLSN* smxTrans::getTransLstUndoNxtLSNPtr(void* aTrans)
{
    return &( ((smxTrans*)aTrans)->mLstUndoNxtLSN );
}

IDE_RC smxTrans::setTransLstUndoNxtLSN( void   * aTrans, 
                                        smLSN    aLSN )
{
    return ((smxTrans*)aTrans)->setLstUndoNxtLSN( aLSN );
}

void smxTrans::getTxIDAnLogType( void    * aTrans, 
                                 smTID   * aTID, 
                                 UInt    * aLogType )
{
    smxTrans* sTrans = (smxTrans*) aTrans;

    *aTID =  sTrans->mTransID;
    *aLogType = sTrans->mLogTypeFlag;
}

idBool smxTrans::getTransAbleToRollback( void  * aTrans )
{
    return ((smxTrans*)aTrans)->mAbleToRollback ;
}

idBool smxTrans::isTxBeginStatus( void  * aTrans )
{
    if ( ((smxTrans*)aTrans)->mStatus == SMX_TX_BEGIN )
    {
        return ID_TRUE;
    }
    else
    {
        return ID_FALSE;
    }
}

/***************************************************************************
 *
 * Description: Startup �������� Verify�� IndexOID�� �߰��Ѵ�.
 *
 * aTrans    [IN] - Ʈ�����
 * aTableOID [IN] - aTableOID
 * aIndexOID [IN] - Verify�� aIndexOID
 * aSpaceID  [IN] - �ش� Tablespace ID
 *
 ***************************************************************************/
IDE_RC smxTrans::addOIDToVerify( void *    aTrans,
                                 smOID     aTableOID,
                                 smOID     aIndexOID,
                                 scSpaceID aSpaceID )
{
    smxTrans  * sTrans;

    IDE_ASSERT( aTrans    != NULL );
    IDE_ASSERT( aTableOID != SM_NULL_OID );
    IDE_ASSERT( aIndexOID != SM_NULL_OID );

    sTrans = (smxTrans*) aTrans;
    IDE_ASSERT( sTrans->mStatus == SMX_TX_BEGIN );

    return sTrans->mOIDToVerify->add( aTableOID,
                                      aIndexOID,
                                      aSpaceID,
                                      SM_OID_NULL /* unuseless */);
}

IDE_RC  smxTrans::addOIDByTID( smTID     aTID,
                               smOID     aTblOID,
                               smOID     aRecordOID,
                               scSpaceID aSpaceID,
                               UInt      aFlag )
{
    smxTrans* sCurTrans;

    sCurTrans = smxTransMgr::getTransByTID(aTID);

    /* BUG-14558:OID List�� ���� Add�� Transaction Begin�Ǿ��� ����
       ����Ǿ�� �Ѵ�.*/
    if (sCurTrans->mStatus == SMX_TX_BEGIN )
    {
        return sCurTrans->addOID(aTblOID,aRecordOID,aSpaceID,aFlag);
    }

    return IDE_SUCCESS;
}

idBool  smxTrans::isXAPreparedCommitState( void  * aTrans )
{
    if ( ((smxTrans*) aTrans)->mCommitState == SMX_XA_PREPARED )
    {
        return ID_TRUE;
    }
    else
    {
        return ID_FALSE;
    }
}

void smxTrans::addMutexToTrans (void *aTrans, void* aMutex)
{
    /* BUG-17569 SM ���ؽ� ���� �������� ���� ���� ���:
       �������� SMX_MUTEX_COUNT(=10) ���� ��Ʈ���� ���� �迭�� ����Ͽ�,
       ���ؽ� ������ SMX_MUTEX_COUNT�� �Ѿ�� ������ ����ߴ�.
       �� ������ ���ְ��� �迭�� ��ũ�帮��Ʈ�� �����Ͽ���. */
    smxTrans *sTrans = (smxTrans*)aTrans;
    smuList  *sNode  = NULL;

    IDE_ASSERT( mMutexListNodePool.alloc((void **)&sNode) == IDE_SUCCESS );
    sNode->mData = (void *)aMutex;
    SMU_LIST_ADD_LAST(&sTrans->mMutexList, sNode);
}

IDE_RC smxTrans::writeLogBufferWithOutOffset(void *aTrans, const void *aLog,
                                             UInt aLogSize)
{
    return ((smxTrans*)aTrans)->writeLogToBuffer(aLog,aLogSize);
}


SChar * smxTrans::getTransLogBuffer (void *aTrans)
{
    return ((smxTrans*)aTrans)->getLogBuffer();
}

UInt   smxTrans::getLogTypeFlagOfTrans(void * aTrans)
{
    return ((smxTrans*) aTrans)->mLogTypeFlag;
}

void   smxTrans::addToUpdateSizeOfTrans (void *aTrans, UInt aSize)
{
    ((smxTrans*) aTrans)->mUpdateSize += aSize;
}


idvSQL * smxTrans::getStatistics( void* aTrans )
{
    return ((smxTrans*)aTrans)->mStatistics;
}

IDE_RC smxTrans::resumeTrans(void * aTrans)
{
    return ((smxTrans*)aTrans)->resume();
}

/*******************************************************************************
 * Description: �� �Լ��� � Ʈ������� Tuple�� SCN�� �о��� ��,
 *      �� ���� ���Ѵ��� ��� �� Tuple�� ���� ������ ������ Ʈ�������
 *      commit �������� �Ǵ��ϴ� �Լ��̴�.
 *      ��, Tuple�� SCN�� ���Ѵ��̴��� �о�߸� �ϴ� ��츦 �˻��ϱ� �����̴�.
 *
 * MMDB:
 *  ���� slotHeader�� mSCN���� ��Ȯ�� ������ �˱� ���� ���Լ��� ȣ���Ѵ�.
 * slotHeader������ Ʈ������� ������ �����ϸ�, �� Ʈ������� �����ϱ� ������
 * slotHeader�� mSCN�� ���Ѵ밪���� �����ǰ�, �� Ʈ������� commit�ϰ� �Ǹ� ��
 * commitSCN�� slotHeader�� ����ǰ� �ȴ�.
 *  �̶� Ʈ������� ���� �ڽ��� commitSCN�� �����ϰ�, �ڽ��� ������ ������
 * slotHeader�� ���� mSCN���� �����ϰ� �ȴ�. ���� �� ���̿� slot�� mSCN����
 * �������� �Ǹ� ������ �߻��� �� �ִ�.
 *  �׷��� ������ slot�� mSCN���� ������ ���Ŀ��� �� �Լ��� �����Ͽ� ��Ȯ�� ����
 * ���´�.
 *  ���� slotHeader�� mSCN���� ���Ѵ���, Ʈ������� commit�Ǿ����� ���θ� ����
 * commit�Ǿ��ٸ� �� commitSCN�� ���� slot�� mSCN�̶�� �����Ѵ�.
 *
 * DRDB:
 *  MMDB�� �����ϰ� Tx�� Commit�� ���Ŀ� TSS�� CommitSCN�� �����Ǵ� ���̿�
 * Record�� CommitSCN�� Ȯ���Ϸ� ���� �� TSS�� ���� CommitSCN�� �����Ǿ� ����
 * ���� �� �ִ�. ���� TSS�� CSCN�� ���Ѵ��� �� �� �Լ��� �ѹ� �� ȣ���ؼ�
 * Tx�κ��� ������ ��Ȯ�� CommitSCN�� �ٽ� Ȯ���Ѵ�.
 *
 * Parameters:
 *  aRecTID        - [IN]  TID on Tuple: read only
 *  aRecSCN        - [IN]  SCN on Tuple or TSSlot: read only
 *  aOutSCN        - [OUT] Output SCN
 ******************************************************************************/
void smxTrans::getTransCommitSCN( smTID         aRecTID,
                                  const smSCN * aRecSCN,
                                  smSCN       * aOutSCN )
{
    smxTrans  *sTrans   = NULL;
    smTID      sObjTID  = 0;
    smxStatus  sStatus  = SMX_TX_BEGIN;
    smSCN      sCommitSCN;
    smSCN      sLegacyTransCommitSCN;

    if ( SM_SCN_IS_NOT_INFINITE( *aRecSCN ) )
    {
        SM_GET_SCN( aOutSCN, aRecSCN );

        IDE_CONT( return_immediately );
    }

    SM_MAX_SCN( &sCommitSCN );
    SM_MAX_SCN( &sLegacyTransCommitSCN );

    /* BUG-45147 */
    ID_SERIAL_BEGIN(sTrans = smxTransMgr::getTransByTID(aRecTID));

    ID_SERIAL_EXEC( sStatus = sTrans->mStatus, 1);

    ID_SERIAL_EXEC( SM_GET_SCN( &sCommitSCN, &(sTrans->mCommitSCN) ), 2 );

    ID_SERIAL_END(sObjTID = sTrans->mTransID);

    if ( aRecTID == sObjTID )
    {
        /* bug-9750 sStatus�� copy�Ҷ���  commit���¿�����
         * sCommitSCN�� copy�ϱ����� tx�� end�ɼ� �ִ�(end�̸�
         * commitSCN�� inifinite�� �ȴ�. */
        if ( (sStatus == SMX_TX_COMMIT) &&
             SM_SCN_IS_NOT_INFINITE(sCommitSCN) )
        {
            if ( SM_SCN_IS_DELETED( *aOutSCN ) )
            {
                SM_SET_SCN_DELETE_BIT( &sCommitSCN );
            }
            else
            {
                /* Do Nothing */
            }

            /* this transaction is committing, so use Tx-CommitSCN */
            SM_SET_SCN(aOutSCN, &sCommitSCN);
        }
        else
        {
            /* Tx�� COMMIT ���°� �ƴϸ� aRecSCN�� infinite SCN�� �Ѱ��ش�. */
            SM_GET_SCN( aOutSCN, aRecSCN );
        }
    }
    else
    {
        /* Legacy Trans���� Ȯ���ϰ� Legacy Transaction�̸�
         * Legacy Trans List���� Ȯ���ؼ� commit SCN�� ��ȯ�Ѵ�. */
        if ( sTrans->mLegacyTransCnt != 0 )
        {
            sLegacyTransCommitSCN = smxLegacyTransMgr::getCommitSCN( aRecTID );

            IDE_ASSERT( SM_SCN_IS_NOT_INFINITE( sLegacyTransCommitSCN ) );
        }

        if ( SM_SCN_IS_MAX( sLegacyTransCommitSCN ) )
        {
            /* already end. another tx is running. */
            SM_GET_SCN( aOutSCN, aRecSCN );
        }
        else
        {
            if ( SM_SCN_IS_DELETED( *aOutSCN ) )
            {
                SM_SET_SCN_DELETE_BIT( &sLegacyTransCommitSCN );
            }
            else
            {
                /* Do Nothing */
            }

            SM_GET_SCN( aOutSCN, &sLegacyTransCommitSCN );
        }
    }

    IDE_EXCEPTION_CONT( return_immediately );
}

/*
 * =============================================================================
 *  ���� statement�� DB���� ���� ��,
 *  Disk only, memory only, disk /memory�� ����  ���� ������ transaction��
 *  memory viewSCN, disk viewSCN�� ���Ͽ�  �� ���� SCN���� �����Ѵ�.
 * =============================================================================
 *                        !!!!!!!!!!! WARNING  !!!!!!!!!!!!!1
 * -----------------------------------------------------------------------------
 *  �Ʒ��� getViewSCN() �Լ��� �ݵ�� SMX_TRANS_LOCKED, SMX_TRANS_UNLOCKED
 *  ���� ���ο��� ����Ǿ�� �Ѵ�. �׷��� ������,
 *  smxTransMgr::getSysMinMemViewSCN()���� �� �Ҵ���� SCN���� SKIP�ϴ� ��찡
 *  �߻��ϰ�, �� ��� Ager�� ������ ������ �ʷ��ϰ� �ȴ�.
 *  (�б� ��� Tuple�� Aging �߻�!!!)
 * =============================================================================
 */
IDE_RC smxTrans::allocViewSCN( UInt    aStmtFlag, smSCN * aStmtSCN )
{
    UInt     sStmtFlag;

    mLSLockFlag = SMX_TRANS_LOCKED;

    IDL_MEM_BARRIER;

    sStmtFlag = aStmtFlag & SMI_STATEMENT_CURSOR_MASK;
    IDE_ASSERT( (sStmtFlag == SMI_STATEMENT_ALL_CURSOR)  ||
                (sStmtFlag == SMI_STATEMENT_DISK_CURSOR) ||
                (sStmtFlag == SMI_STATEMENT_MEMORY_CURSOR) );

    // BUG-26881 �߸��� CTS stamping���� acces�� �� ���� row�� ������
    // Ʈ����� begin�� active Ʈ����� �� oldestFstViewSCN�� ������
    if ( SM_SCN_IS_INFINITE( mOldestFstViewSCN ) )
    {
        if ( (sStmtFlag == SMI_STATEMENT_ALL_CURSOR) ||
             (sStmtFlag == SMI_STATEMENT_DISK_CURSOR) )
        {
            smxTransMgr::getSysMinDskFstViewSCN( &mOldestFstViewSCN );
        }
        else
        {
            /* nothing to do */
        }
    }
    else
    {
        /* nothing to do */
    }

    smxTransMgr::mGetSmmViewSCN( aStmtSCN );

    gSmxTrySetupViewSCNFuncs[sStmtFlag]( this, aStmtSCN );

    IDL_MEM_BARRIER;

    mLSLockFlag = SMX_TRANS_UNLOCKED;

    return IDE_SUCCESS;
}

/*****************************************************************
 *
 * Description: Ʈ������� MinDskViewSCN Ȥ�� MinMemViewSCN Ȥ�� �Ѵ�
 *              ���� �õ��Ѵ�.
 *
 * aTrans   - [IN] Ʈ����� ������
 * aViewSCN - [IN] Stmt�� ViewSCN
 *
 *****************************************************************/
void smxTrans::trySetupMinAllViewSCN( void   * aTrans,
                                      smSCN  * aViewSCN )
{
    trySetupMinMemViewSCN( aTrans, aViewSCN );

    trySetupMinDskViewSCN( aTrans, aViewSCN );
}


/*****************************************************************
 *
 * Description: Ʈ������� ������ DskStmt��� Ʈ������� MinDskViewSCN��
 *              ���Žõ��Ѵ�.
 *
 * ���� Ʈ����ǿ� MinDskViewSCN�� INFINITE �����Ǿ� �ִٴ� ���� �ٸ� DskStmt
 * �� �������� �ʴ´ٴ� ���� �ǹ��ϸ� �̶�, MinDskViewSCN�� �����Ѵ�.
 * �� FstDskViewSCN�� Ʈ������� ù��° DskStmt�� SCN���� �������־�� �Ѵ�.
 *
 * �̹� �ٸ� DskStmt�� �����Ѵ�.
 *
 * aTrans      - [IN] Ʈ����� ������
 * aDskViewSCN - [IN] DskViewSCN
 *
 *********************************************************************/
void smxTrans::trySetupMinDskViewSCN( void   * aTrans,
                                      smSCN  * aDskViewSCN )
{
    smxTrans* sTrans = (smxTrans*)aTrans;

    IDE_ASSERT( SM_SCN_IS_NOT_INFINITE( *aDskViewSCN ) );

    /* DskViewSCN���� �����Ѵ�. �̹� ���Լ��� call�ϴ� �ֻ��� �Լ���
     * smxTrans::allocViewSCN���� mLSLockFlag�� SMX_TRANS_LOCKED�� �����Ͽ���
     * setSCN��ſ� �ٷ� SM_SET_SCN�� �̿��Ѵ�. */
    if ( SM_SCN_IS_INFINITE(sTrans->mMinDskViewSCN) )
    {
        SM_SET_SCN( &sTrans->mMinDskViewSCN, aDskViewSCN );

        if ( SM_SCN_IS_INFINITE( sTrans->mFstDskViewSCN ) )
        {
            /* Ʈ������� ù��° Disk Stmt�� SCN�� �����Ѵ�. */
            SM_SET_SCN( &sTrans->mFstDskViewSCN, aDskViewSCN );
        }
    }
    else
    {
        /* �̹� �ٸ� DskStmt�� �����ϴ� ����̰� ���� SCN�� �� DskStmt��
         * SCN���� ���ų� ū ��츸 �����Ѵ�. */
        IDE_ASSERT( SM_SCN_IS_GE( aDskViewSCN,
                                  &sTrans->mMinDskViewSCN ) );
    }
}

/*****************************************************************
 *
 * Description: Ʈ������� ������ MemStmt��� Ʈ������� MinMemViewSCN��
 *              ���Žõ��Ѵ�.
 *
 * ���� Ʈ����ǿ� MinMemViewSCN�� INFINITE �����Ǿ� �ִٴ� ���� �ٸ� MemStmt
 * �� �������� �ʴ´ٴ� ���� �ǹ��ϸ� �̶�, MinMemViewSCN�� �����Ѵ�.
 * �� FstMemViewSCN�� Ʈ������� ù��° MemStmt�� SCN���� �������־�� �Ѵ�.
 *
 * �̹� �ٸ� MemStmt�� �����Ѵ�.
 *
 * aTrans      - [IN] Ʈ����� ������
 * aMemViewSCN - [IN] MemViewSCN
 *
 *****************************************************************/
void smxTrans::trySetupMinMemViewSCN( void  * aTrans,
                                      smSCN * aMemViewSCN )
{
    smxTrans* sTrans = (smxTrans*) aTrans;

    IDE_ASSERT( SM_SCN_IS_NOT_INFINITE( *aMemViewSCN ) );

    /* MemViewSCN���� �����Ѵ�. �̹� ���Լ��� call�ϴ� �ֻ��� �Լ���
     * smxTrans::allocViewSCN���� mLSLockFlag�� SMX_TRANS_LOCKED��
     * �����Ͽ��� setSCN��ſ� �ٷ� SM_SET_SCN�� �̿��Ѵ�. */
    if ( SM_SCN_IS_INFINITE( sTrans->mMinMemViewSCN) )
    {
        SM_SET_SCN( &sTrans->mMinMemViewSCN, aMemViewSCN );
    }
    else
    {
        /* �̹� �ٸ� MemStmt�� �����ϴ� ����̰� ���� SCN�� �� MemStmt��
         * SCN���� ���ų� ū ��츸 �����Ѵ�. */
        IDE_ASSERT( SM_SCN_IS_GE( aMemViewSCN,
                                  &sTrans->mMinMemViewSCN ) );
    }
}


/* <<CALLBACK FUNCTION>>
 * �ǵ� : commit�� �����ϴ� Ʈ������� CommitSCN�� �Ҵ���� �Ŀ�
 *        �ڽ��� ���¸� �Ʒ��� ���� ������ �����ؾ� �ϸ�, �̸� ����
 *        �Լ� smmManager::getCommitSCN()���� callback���� ȣ��ȴ�.
 */

void smxTrans::setTransCommitSCN(void      *aTrans,
                                 smSCN      aSCN,
                                 void      *aStatus)
{
    smxTrans *sTrans = (smxTrans *)aTrans;

    SM_SET_SCN(&(sTrans->mCommitSCN), &aSCN);
    IDL_MEM_BARRIER;
    sTrans->mStatus = *(smxStatus *)aStatus;
}

/**********************************************************************
 *
 * Description : ù��° Disk Stmt�� ViewSCN�� �����Ѵ�.
 *
 * aTrans         - [IN] Ʈ����� ������
 * aFstDskViewSCN - [IN] ù��° Disk Stmt�� ViewSCN
 *
 **********************************************************************/
void smxTrans::setFstDskViewSCN( void  * aTrans,
                                 smSCN * aFstDskViewSCN )
{
    SM_SET_SCN( &((smxTrans*)aTrans)->mFstDskViewSCN,
                aFstDskViewSCN );
}

/**********************************************************************
 *
 * Description : system���� ��� active Ʈ����ǵ���
 *               oldestFstViewSCN�� ��ȯ
 *     BY  BUG-26881 �߸��� CTS stamping���� acces�� �� ���� row�� ������
 *
 * aTrans - [IN] Ʈ����� ������
 *
 **********************************************************************/
smSCN smxTrans::getOldestFstViewSCN( void * aTrans )
{
    smSCN   sOldestFstViewSCN;

    SM_GET_SCN( &sOldestFstViewSCN, &((smxTrans*)aTrans)->mOldestFstViewSCN );

    return sOldestFstViewSCN;
}

// for  fix bug-8084.
IDE_RC smxTrans::begin4LayerCall(void   * aTrans,
                                 UInt     aFlag,
                                 idvSQL * aStatistics)
{
    IDE_ASSERT( aTrans != NULL );

    return  ((smxTrans*)aTrans)->begin(aStatistics,
                                       aFlag,
                                       SMX_NOT_REPL_TX_ID);

}
// for  fix bug-8084
IDE_RC smxTrans::abort4LayerCall(void* aTrans)
{

    IDE_ASSERT( aTrans != NULL );

    return  ((smxTrans*)aTrans)->abort();
}
// for  fix bug-8084.
IDE_RC smxTrans::commit4LayerCall(void* aTrans)
{

    smSCN  sDummySCN;
    IDE_ASSERT( aTrans != NULL );

    return  ((smxTrans*)aTrans)->commit(&sDummySCN);
}

/***********************************************************************
 *
 * Description : Implicit Savepoint�� ����Ѵ�. Implicit SVP�� sStamtDepth
 *               �� ���� ����Ѵ�.
 *
 * aSavepoint     - [IN] Savepoint
 * aStmtDepth     - [IN] Statement Depth
 *
 ***********************************************************************/
IDE_RC smxTrans::setImpSavepoint( smxSavepoint **aSavepoint,
                                  UInt           aStmtDepth)
{
    return mSvpMgr.setImpSavepoint( aSavepoint,
                                    aStmtDepth,
                                    mOIDList->mOIDNodeListHead.mPrvNode,
                                    &mLstUndoNxtLSN,
                                    svrLogMgr::getLastLSN( &mVolatileLogEnv ),
                                    smLayerCallback::getLastLockSequence( mSlotN ) );
}

/***********************************************************************
 * Description : Statement�� ����� �����ߴ� Implicit SVP�� Implici SVP
 *               List���� �����Ѵ�.
 *
 * aSavepoint     - [IN] Savepoint
 * aStmtDepth     - [IN] Statement Depth
 ***********************************************************************/
IDE_RC smxTrans::unsetImpSavepoint( smxSavepoint *aSavepoint )
{
    return mSvpMgr.unsetImpSavepoint( aSavepoint );
}

IDE_RC smxTrans::setExpSavepoint(const SChar *aExpSVPName)
{
    return mSvpMgr.setExpSavepoint( this,
                                    aExpSVPName,
                                    mOIDList->mOIDNodeListHead.mPrvNode,
                                    &mLstUndoNxtLSN,
                                    svrLogMgr::getLastLSN( &mVolatileLogEnv ),
                                    smLayerCallback::getLastLockSequence( mSlotN ) );
}

void smxTrans::reservePsmSvp( )
{
    mSvpMgr.reservePsmSvp( mOIDList->mOIDNodeListHead.mPrvNode,
                           &mLstUndoNxtLSN,
                           svrLogMgr::getLastLSN( &mVolatileLogEnv ),
                           smLayerCallback::getLastLockSequence( mSlotN ) );
};

/*****************************************************************
 * Description: Transaction Status Slot �Ҵ�
 *
 * [ ���� ]
 *
 * ��ũ ���� Ʈ������� TSS�� �ʿ��ϰ�, UndoRow�� ����ϱ⵵
 * �ؾ��ϱ� ������ Ʈ����� ���׸�Ʈ ��Ʈ���� �Ҵ��Ͽ� TSS���׸�Ʈ��
 * ��� ���׸�Ʈ�� Ȯ���ؾ� �Ѵ�.
 *
 * Ȯ���� TSS ���׸�Ʈ�κ��� TSS�� �Ҵ��Ͽ� Ʈ����ǿ� �����Ѵ�.
 *
 * [ ���� ]
 *
 * aStatistics      - [IN] �������
 * aStartInfo       - [IN] Ʈ����� �� �α�����
 *
 *****************************************************************/
IDE_RC smxTrans::allocTXSegEntry( idvSQL          * aStatistics,
                                  sdrMtxStartInfo * aStartInfo )
{
    smxTrans      * sTrans;
    sdcTXSegEntry * sTXSegEntry;
    UInt            sManualBindingTxSegByEntryID;

    IDE_ASSERT( aStartInfo != NULL );

    sTrans = (smxTrans*)aStartInfo->mTrans;

    if ( sTrans->mTXSegEntry == NULL )
    {
        IDE_ASSERT( sTrans->mTXSegEntryIdx == ID_UINT_MAX );

        sManualBindingTxSegByEntryID = smuProperty::getManualBindingTXSegByEntryID();

        // BUG-29839 ����� undo page���� ���� CTS�� ������ �� �� ����.
        // �����ϱ� ���� transaction�� Ư�� segment entry�� binding�ϴ� ��� �߰�
        if ( sManualBindingTxSegByEntryID ==
             SMX_AUTO_BINDING_TRANSACTION_SEGMENT_ENTRY )
        {
            IDE_TEST( sdcTXSegMgr::allocEntry( aStatistics, 
                                               aStartInfo,
                                               &sTXSegEntry )
                      != IDE_SUCCESS );
        }
        else
        {
            IDE_TEST( sdcTXSegMgr::allocEntryBySegEntryID(
                                              sManualBindingTxSegByEntryID,
                                              &sTXSegEntry )
                      != IDE_SUCCESS );
        }

        sTrans->mTXSegEntryIdx = sTXSegEntry->mEntryIdx;
        sTrans->mTXSegEntry    = sTXSegEntry;

        /*
         * [BUG-27542] [SD] TSS Page Allocation ���� �Լ�(smxTrans::allocTXSegEntry,
         *             sdcTSSegment::bindTSS)���� Exceptionó���� �߸��Ǿ����ϴ�.
         */
        IDU_FIT_POINT( "1.BUG-27542@smxTrans::allocTXSegEntry" );
    }
    else
    {
        sTXSegEntry = sTrans->mTXSegEntry;
    }

    /*
     * �ش� Ʈ������� �ѹ��� Bind�� ���� ���ٸ� bindTSS�� �����Ѵ�.
     */
    if ( sTXSegEntry->mTSSlotSID == SD_NULL_SID )
    {
        IDE_TEST( sdcTXSegMgr::getTSSegPtr(sTXSegEntry)->bindTSS( aStatistics,
                                                                  aStartInfo )
                  != IDE_SUCCESS );
    }

    IDE_ASSERT( sTXSegEntry->mExtRID4TSS != SD_NULL_RID );
    IDE_ASSERT( sTXSegEntry->mTSSlotSID  != SD_NULL_SID );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

IDE_RC smxTrans::executePendingList( idBool aIsCommit )
{

    smuList *sOpNode;
    smuList *sBaseNode;
    smuList *sNextNode;

    sBaseNode = &mPendingOp;

    for ( sOpNode = SMU_LIST_GET_FIRST(sBaseNode);
           sOpNode != sBaseNode;
           sOpNode = sNextNode )
    {
        IDE_TEST( sctTableSpaceMgr::executePendingOperation( mStatistics,
                                                             sOpNode->mData,
                                                             aIsCommit)
                  != IDE_SUCCESS );

        sNextNode = SMU_LIST_GET_NEXT(sOpNode);

        SMU_LIST_DELETE(sOpNode);

        IDE_TEST( iduMemMgr::free(sOpNode->mData) != IDE_SUCCESS );

        IDE_TEST( iduMemMgr::free(sOpNode) != IDE_SUCCESS );
    }

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;

}

/******************************************************
 * Description: pending operation�� pending list�� �߰�
 *******************************************************/
void  smxTrans::addPendingOperation( void*    aTrans,
                                     smuList* aPendingOp )
{

    smxTrans *sTrans;

    IDE_DASSERT( aTrans != NULL );
    IDE_DASSERT( aPendingOp != NULL );

    sTrans = (smxTrans *)aTrans;
    SMU_LIST_ADD_LAST(&(sTrans->mPendingOp), aPendingOp);

    return;

}

/**********************************************************************
 * Tx's PrivatePageList�� ��ȯ�Ѵ�.
 *
 * aTrans           : �۾���� Ʈ����� ��ü
 * aTableOID        : �۾���� ���̺� OID
 * aPrivatePageList : ��ȯ�Ϸ��� PrivatePageList ������
 **********************************************************************/
IDE_RC smxTrans::findPrivatePageList(
                            void*                     aTrans,
                            smOID                     aTableOID,
                            smpPrivatePageListEntry** aPrivatePageList )
{
    smxTrans* sTrans = (smxTrans*)aTrans;

    IDE_DASSERT( aTrans != NULL );

    *aPrivatePageList = NULL;

#ifdef DEBUG
    if ( sTrans->mPrivatePageListCachePtr == NULL )
    {
        // Cache�� PrivatePageList�� ����ٸ� HashTable���� ����� �Ѵ�.
        IDE_TEST( smuHash::findNode( &(sTrans->mPrivatePageListHashTable),
                                     &aTableOID,
                                     (void**)aPrivatePageList )
                  != IDE_SUCCESS );

        IDE_DASSERT( *aPrivatePageList == NULL );
    }
#endif /* DEBUG */

    if ( sTrans->mPrivatePageListCachePtr != NULL )
    {
        // cache�� PrivatePageList���� �˻��Ѵ�.
        if ( sTrans->mPrivatePageListCachePtr->mTableOID == aTableOID )
        {
            *aPrivatePageList = sTrans->mPrivatePageListCachePtr;
        }
        else
        {
            // cache�� PrivatePageList�� �ƴ϶�� HashTable�� �˻��Ѵ�.
            IDE_TEST( smuHash::findNode( &(sTrans->mPrivatePageListHashTable),
                                         &aTableOID,
                                         (void**)aPrivatePageList )
                      != IDE_SUCCESS );

            if ( *aPrivatePageList != NULL )
            {
                // ���� ã�� PrivatePageList�� cache�Ѵ�.
                sTrans->mPrivatePageListCachePtr = *aPrivatePageList;
            }
        }
    }


    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/**********************************************************************
 * Tx's PrivatePageList�� ��ȯ�Ѵ�.
 *
 * aTrans           : �۾���� Ʈ����� ��ü
 * aTableOID        : �۾���� ���̺� OID
 * aPrivatePageList : ��ȯ�Ϸ��� PrivatePageList ������
 **********************************************************************/
IDE_RC smxTrans::findVolPrivatePageList(
                            void*                     aTrans,
                            smOID                     aTableOID,
                            smpPrivatePageListEntry** aPrivatePageList )
{
    smxTrans* sTrans = (smxTrans*)aTrans;

    IDE_DASSERT( aTrans != NULL );

    *aPrivatePageList = NULL;

#ifdef DEBUG
    if ( sTrans->mVolPrivatePageListCachePtr == NULL )
    {
        // Cache�� PrivatePageList�� ����ٸ� HashTable���� ����� �Ѵ�.
        IDE_TEST( smuHash::findNode( &(sTrans->mVolPrivatePageListHashTable),
                                     &aTableOID,
                                     (void**)aPrivatePageList )
                  != IDE_SUCCESS );

        IDE_DASSERT( *aPrivatePageList == NULL );
    }
#endif /* DEBUG */

    if ( sTrans->mVolPrivatePageListCachePtr != NULL )
    {
        // cache�� PrivatePageList���� �˻��Ѵ�.
        if ( sTrans->mVolPrivatePageListCachePtr->mTableOID == aTableOID )
        {
            *aPrivatePageList = sTrans->mVolPrivatePageListCachePtr;
        }
        else
        {
            // cache�� PrivatePageList�� �ƴ϶�� HashTable�� �˻��Ѵ�.
            IDE_TEST( smuHash::findNode( &(sTrans->mVolPrivatePageListHashTable),
                                         &aTableOID,
                                         (void**)aPrivatePageList )
                      != IDE_SUCCESS );

            if ( *aPrivatePageList != NULL )
            {
                // ���� ã�� PrivatePageList�� cache�Ѵ�.
                sTrans->mVolPrivatePageListCachePtr = *aPrivatePageList;
            }
        }
    }

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/**********************************************************************
 * Tx's PrivatePageList�� �߰��Ѵ�.
 *
 * aTrans           : �۾���� Ʈ����� ��ü
 * aTableOID        : �۾���� ���̺� OID
 * aPrivatePageList : �߰��Ϸ��� PrivatePageList
 **********************************************************************/

IDE_RC smxTrans::addPrivatePageList(
                                void*                    aTrans,
                                smOID                    aTableOID,
                                smpPrivatePageListEntry* aPrivatePageList )
{
    smxTrans* sTrans = (smxTrans*)aTrans;

    IDE_DASSERT( aTrans != NULL );
    IDE_DASSERT( aPrivatePageList != NULL );
    IDE_DASSERT( aPrivatePageList != sTrans->mPrivatePageListCachePtr);

    IDE_TEST( smuHash::insertNode( &(sTrans->mPrivatePageListHashTable),
                                   &aTableOID,
                                   aPrivatePageList)
              != IDE_SUCCESS );

    // ���� �Էµ� PrivatePageList�� cache�Ѵ�.
    sTrans->mPrivatePageListCachePtr = aPrivatePageList;


    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/**********************************************************************
 * Tx's PrivatePageList�� �߰��Ѵ�.
 *
 * aTrans           : �۾���� Ʈ����� ��ü
 * aTableOID        : �۾���� ���̺� OID
 * aPrivatePageList : �߰��Ϸ��� PrivatePageList
 **********************************************************************/
IDE_RC smxTrans::addVolPrivatePageList(
                                void*                    aTrans,
                                smOID                    aTableOID,
                                smpPrivatePageListEntry* aPrivatePageList )
{
    smxTrans* sTrans = (smxTrans*)aTrans;

    IDE_DASSERT( aTrans != NULL );
    IDE_DASSERT( aPrivatePageList != NULL );
    IDE_DASSERT( aPrivatePageList != sTrans->mVolPrivatePageListCachePtr );

    IDE_TEST( smuHash::insertNode( &(sTrans->mVolPrivatePageListHashTable),
                                   &aTableOID,
                                   aPrivatePageList)
              != IDE_SUCCESS );

    // ���� �Էµ� PrivatePageList�� cache�Ѵ�.
    sTrans->mVolPrivatePageListCachePtr = aPrivatePageList;

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/**********************************************************************
 * PrivatePageList ����
 *
 * aTrans           : �߰��Ϸ��� Ʈ�����
 * aTableOID        : �߰��Ϸ��� PrivatePageList�� ���̺� OID
 * aPrivatePageList : �߰��Ϸ��� PrivateFreePageList�� ������
 **********************************************************************/
IDE_RC smxTrans::createPrivatePageList(
                            void                      * aTrans,
                            smOID                       aTableOID,
                            smpPrivatePageListEntry  ** aPrivatePageList )
{
    UInt      sVarIdx;
    smxTrans* sTrans = (smxTrans*)aTrans;

    IDE_DASSERT( aTrans != NULL );
    IDE_DASSERT( aPrivatePageList != NULL );

    /* smxTrans_createPrivatePageList_alloc_PrivatePageList.tc */
    IDU_FIT_POINT("smxTrans::createPrivatePageList::alloc::PrivatePageList");
    IDE_TEST( sTrans->mPrivatePageListMemPool.alloc((void**)aPrivatePageList)
              != IDE_SUCCESS );

    (*aPrivatePageList)->mTableOID          = aTableOID;
    (*aPrivatePageList)->mFixedFreePageHead = NULL;
    (*aPrivatePageList)->mFixedFreePageTail = NULL;

    for(sVarIdx = 0;
        sVarIdx < SM_VAR_PAGE_LIST_COUNT;
        sVarIdx++)
    {
        (*aPrivatePageList)->mVarFreePageHead[sVarIdx] = NULL;
    }

    IDE_TEST( addPrivatePageList(aTrans,
                                 aTableOID,
                                 *aPrivatePageList)
              != IDE_SUCCESS );


    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    IDE_CALLBACK_FATAL("Failure of Create Tx's Private Page List");

    return IDE_FAILURE;
}

/**********************************************************************
 * PrivatePageList ����
 *
 * aTrans           : �߰��Ϸ��� Ʈ�����
 * aTableOID        : �߰��Ϸ��� PrivatePageList�� ���̺� OID
 * aPrivatePageList : �߰��Ϸ��� PrivateFreePageList�� ������
 **********************************************************************/
IDE_RC smxTrans::createVolPrivatePageList(
                            void                      * aTrans,
                            smOID                       aTableOID,
                            smpPrivatePageListEntry  ** aPrivatePageList )
{
    UInt      sVarIdx;
    smxTrans* sTrans = (smxTrans*)aTrans;

    IDE_DASSERT( aTrans != NULL );
    IDE_DASSERT( aPrivatePageList != NULL );

    /* smxTrans_createVolPrivatePageList_alloc_PrivatePageList.tc */
    IDU_FIT_POINT("smxTrans::createVolPrivatePageList::alloc::PrivatePageList");
    IDE_TEST( sTrans->mVolPrivatePageListMemPool.alloc((void**)aPrivatePageList)
              != IDE_SUCCESS );

    (*aPrivatePageList)->mTableOID          = aTableOID;
    (*aPrivatePageList)->mFixedFreePageHead = NULL;
    (*aPrivatePageList)->mFixedFreePageTail = NULL;

    for(sVarIdx = 0;
        sVarIdx < SM_VAR_PAGE_LIST_COUNT;
        sVarIdx++)
    {
        (*aPrivatePageList)->mVarFreePageHead[sVarIdx] = NULL;
    }

    IDE_TEST( addVolPrivatePageList(aTrans,
                                    aTableOID,
                                   *aPrivatePageList)
              != IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    IDE_CALLBACK_FATAL("Failure of Create Tx's Private Page List");

    return IDE_FAILURE;
}

/**********************************************************************
 * Tx's PrivatePageList�� ��� FreePage���� ���̺��� ������,
 * PrivatePageList�� �����Ѵ�.
 **********************************************************************/
IDE_RC smxTrans::finAndInitPrivatePageList()
{
    smcTableHeader*          sTableHeader;
    UInt                     sVarIdx;
    smpPrivatePageListEntry* sPrivatePageList = NULL;

    // PrivatePageList�� HashTable���� �ϳ��� ���� �����´�.
    IDE_TEST( smuHash::open(&mPrivatePageListHashTable) != IDE_SUCCESS );
    IDE_TEST( smuHash::cutNode( &mPrivatePageListHashTable,
                                (void**)&sPrivatePageList )
             != IDE_SUCCESS );

    IDE_DASSERT( ( mPrivatePageListCachePtr != NULL &&
                   sPrivatePageList != NULL ) ||
                 ( mPrivatePageListCachePtr == NULL &&
                   sPrivatePageList == NULL ) );

    // merge Private Page List To TABLE
    while ( sPrivatePageList != NULL )
    {
        IDE_ASSERT( smcTable::getTableHeaderFromOID( 
                                            sPrivatePageList->mTableOID,
                                            (void**)&sTableHeader )
                    == IDE_SUCCESS );

        // for FixedEntry
        if ( sPrivatePageList->mFixedFreePageHead != NULL )
        {
            IDE_TEST( smpFreePageList::addFreePagesToTable(
                                        this,
                                        &(sTableHeader->mFixed.mMRDB),
                                        sPrivatePageList->mFixedFreePageHead)
                      != IDE_SUCCESS );
        }

        // for VarEntry
        for(sVarIdx = 0;
            sVarIdx < SM_VAR_PAGE_LIST_COUNT;
            sVarIdx++)
        {
            if ( sPrivatePageList->mVarFreePageHead[sVarIdx] != NULL )
            {
                IDE_TEST( smpFreePageList::addFreePagesToTable(
                                     this,
                                     &(sTableHeader->mVar.mMRDB[sVarIdx]),
                                     sPrivatePageList->mVarFreePageHead[sVarIdx])
                         != IDE_SUCCESS );
            }
        }

        IDE_TEST( mPrivatePageListMemPool.memfree(sPrivatePageList)
                  != IDE_SUCCESS );

        // PrivatePageList�� HashTable���� ���� ���� �����´�.
        IDE_TEST( smuHash::cutNode( &mPrivatePageListHashTable,
                                    (void**)&sPrivatePageList )
                  != IDE_SUCCESS );
    }

    // PrivatePageList�� HashTable�� ���ƴٸ� �����Ѵ�.
    IDE_TEST( smuHash::close(&mPrivatePageListHashTable) != IDE_SUCCESS );

    mPrivatePageListCachePtr = NULL;

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    IDE_CALLBACK_FATAL("Failure of Destroy Tx's Private Page List");

    return IDE_FAILURE;
}

/**********************************************************************
 * Tx's PrivatePageList�� ��� FreePage���� ���̺��� ������,
 * PrivatePageList�� �����Ѵ�.
 **********************************************************************/
IDE_RC smxTrans::finAndInitVolPrivatePageList()
{
    smcTableHeader*          sTableHeader;
    UInt                     sVarIdx;
    smpPrivatePageListEntry* sPrivatePageList = NULL;

    // PrivatePageList�� HashTable���� �ϳ��� ���� �����´�.
    IDE_TEST( smuHash::open(&mVolPrivatePageListHashTable) != IDE_SUCCESS );
    IDE_TEST( smuHash::cutNode( &mVolPrivatePageListHashTable,
                                (void**)&sPrivatePageList )
              != IDE_SUCCESS );

    IDE_DASSERT( ( mVolPrivatePageListCachePtr != NULL &&
                   sPrivatePageList != NULL ) ||
                 ( mVolPrivatePageListCachePtr == NULL &&
                   sPrivatePageList == NULL ) );

    // merge Private Page List To TABLE
    while ( sPrivatePageList != NULL )
    {
        IDE_ASSERT( smcTable::getTableHeaderFromOID( 
                                            sPrivatePageList->mTableOID,
                                            (void**)&sTableHeader )
                    == IDE_SUCCESS );

        // for FixedEntry
        if ( sPrivatePageList->mFixedFreePageHead != NULL )
        {
            IDE_TEST( svpFreePageList::addFreePagesToTable(
                                     this,
                                     &(sTableHeader->mFixed.mVRDB),
                                     sPrivatePageList->mFixedFreePageHead)
                     != IDE_SUCCESS );
        }

        // for VarEntry
        for(sVarIdx = 0;
            sVarIdx < SM_VAR_PAGE_LIST_COUNT;
            sVarIdx++)
        {
            if ( sPrivatePageList->mVarFreePageHead[sVarIdx] != NULL )
            {
                IDE_TEST( svpFreePageList::addFreePagesToTable(
                                     this,
                                     &(sTableHeader->mVar.mVRDB[sVarIdx]),
                                     sPrivatePageList->mVarFreePageHead[sVarIdx])
                          != IDE_SUCCESS );
            }
        }

        IDE_TEST( mVolPrivatePageListMemPool.memfree(sPrivatePageList)
                  != IDE_SUCCESS );

        // PrivatePageList�� HashTable���� ���� ���� �����´�.
        IDE_TEST( smuHash::cutNode( &mVolPrivatePageListHashTable,
                                   (void**)&sPrivatePageList )
                 != IDE_SUCCESS );
    }

    // PrivatePageList�� HashTable�� ���ƴٸ� �����Ѵ�.
    IDE_TEST( smuHash::close(&mVolPrivatePageListHashTable) != IDE_SUCCESS );

    mVolPrivatePageListCachePtr = NULL;

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    IDE_CALLBACK_FATAL("Failure of Destroy Tx's Private Page List");

    return IDE_FAILURE;
}


/**********************************************************************
 * BUG-30871 When excuting ALTER TABLE in MRDB, the Private Page Lists of
 * new and old table are registered twice.
 * PrivatePageList�� ������ϴ�.
 * �� �������� �̹� �ش� Page���� TableSpace�� ��ȯ�� �����̱� ������
 * Page�� ���� �ʽ��ϴ�.
 **********************************************************************/
IDE_RC smxTrans::dropMemAndVolPrivatePageList(void           * aTrans,
                                              smcTableHeader * aSrcHeader )
{
    smxTrans               * sTrans;
    smpPrivatePageListEntry* sPrivatePageList = NULL;
    smpPrivatePageListEntry* sVolPrivatePageList = NULL;

    sTrans = (smxTrans*)aTrans;

    IDE_TEST( smuHash::findNode( &sTrans->mPrivatePageListHashTable,
                                 &aSrcHeader->mSelfOID,
                                 (void**)&sPrivatePageList )
              != IDE_SUCCESS );
    if ( sPrivatePageList != NULL )
    {
        IDE_TEST( smuHash::deleteNode( 
                                    &sTrans->mPrivatePageListHashTable,
                                    &aSrcHeader->mSelfOID,
                                    (void**)&sPrivatePageList )
                   != IDE_SUCCESS );

        if ( sTrans->mPrivatePageListCachePtr == sPrivatePageList )
        {
            sTrans->mPrivatePageListCachePtr = NULL;
        }

        /* BUG-34384  
         * If an exception occurs while changing memory or vlatile
         * table(alter table) the server is abnormal shutdown
         */
        IDE_TEST( sTrans->mPrivatePageListMemPool.memfree(sPrivatePageList)
                  != IDE_SUCCESS );

    }

    IDE_TEST( smuHash::findNode( &sTrans->mVolPrivatePageListHashTable,
                                 &aSrcHeader->mSelfOID,
                                 (void**)&sVolPrivatePageList )
              != IDE_SUCCESS );

    if ( sVolPrivatePageList != NULL )
    {
        IDE_TEST( smuHash::deleteNode( 
                                &sTrans->mVolPrivatePageListHashTable,
                                &aSrcHeader->mSelfOID,
                                (void**)&sVolPrivatePageList )
                   != IDE_SUCCESS );

        if ( sTrans->mVolPrivatePageListCachePtr == sVolPrivatePageList )
        {
            sTrans->mVolPrivatePageListCachePtr = NULL;
        }

        /* BUG-34384  
         * If an exception occurs while changing memory or vlatile
         * table(alter table) the server is abnormal shutdown
         */
        IDE_TEST( sTrans->mVolPrivatePageListMemPool.memfree(sVolPrivatePageList)
                  != IDE_SUCCESS );
    }


    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    IDE_CALLBACK_FATAL("Failure of Destroy Tx's Private Page List");

    return IDE_FAILURE;
}



IDE_RC smxTrans::undoInsertOfTableInfo(void* aTrans, smOID aOIDTable)
{
    return (((smxTrans*)aTrans)->undoInsert(aOIDTable));
}

IDE_RC smxTrans::undoDeleteOfTableInfo(void* aTrans, smOID aOIDTable)
{
    return (((smxTrans*)aTrans)->undoDelete(aOIDTable));
}

void smxTrans::updateSkipCheckSCN(void * aTrans,idBool aDoSkipCheckSCN)
{
    ((smxTrans*)aTrans)->mDoSkipCheckSCN  = aDoSkipCheckSCN;
}

// Ư�� Transaction�� RSID�� �����´�.
UInt smxTrans::getRSGroupID(void* aTrans)
{
    smxTrans *sTrans  = (smxTrans*)aTrans;
    UInt sRSGroupID ;

    // SCN�α�, Temp table �α�, ����� ��� aTrans�� NULL�� �� �ִ�.
    if ( sTrans == NULL )
    {
        // 0�� RSID�� ���
        sRSGroupID = 0 ;
    }
    else
    {
        sRSGroupID = sTrans->mRSGroupID ;
    }

    return sRSGroupID;
}

/*
 * Ư�� Transaction�� RSID�� aIdx�� �ٲ۴�.
 *
 * aTrans       [IN]  Ʈ����� ��ü
 * aIdx         [IN]  Resource ID
 */
void smxTrans:: setRSGroupID(void* aTrans, UInt aIdx)
{
    smxTrans *sTrans  = (smxTrans*)aTrans;
    sTrans->mRSGroupID = aIdx;
}

/*
 * Ư�� Transaction�� RSID�� �ο��Ѵ�.
 *
 * 0 < ����Ʈ ID < Page List Count�� �ο��ȴ�.
 *
 * aTrans       [IN]  Ʈ����� ��ü
 * aPageListIdx [OUT] Ʈ����ǿ��� �Ҵ�� Page List ID
 */
void smxTrans::allocRSGroupID(void             *aTrans,
                              UInt             *aPageListIdx)
{
    UInt              sAllocPageListID;
    smxTrans         *sTrans  = (smxTrans*)aTrans;
    static UInt       sPageListCnt = smuProperty::getPageListGroupCount();

    if ( aTrans == NULL )
    {
        // Temp TABLE�� ��� aTrans�� NULL�̴�.
        sAllocPageListID = 0;
    }
    else
    {
        sAllocPageListID = sTrans->mRSGroupID;

        if ( sAllocPageListID == SMP_PAGELISTID_NULL )
        {
            sAllocPageListID = mAllocRSIdx++ % sPageListCnt;

            sTrans->mRSGroupID = sAllocPageListID;
        }
        else
        {
            /* nothing to do ..  */
        }
    }

    if ( aPageListIdx != NULL )
    {
        *aPageListIdx = sAllocPageListID;
    }

}


/*
 * Ư�� Transaction�� �α� ������ ������ �α����Ͽ� ����Ѵ�.
 *
 * aTrans  [IN] Ʈ����� ��ü
 */
IDE_RC smxTrans::writeTransLog(void *aTrans )
{
    smxTrans     *sTrans;

    IDE_DASSERT( aTrans != NULL );
    // IDE_DASSERT( aHeader != NULL );

    sTrans     = (smxTrans*)aTrans;

    // Ʈ����� �α� ������ �α׸� ���Ϸ� ����Ѵ�.
    IDE_TEST( smrLogMgr::writeLog( smxTrans::getStatistics( aTrans ),
                                   aTrans,
                                   sTrans->mLogBuffer,
                                   NULL,  // Previous LSN Ptr
                                   NULL,  // Log LSN Ptr
                                   NULL ) // End LSN Ptr
              != IDE_SUCCESS );


    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*
   callback���� ����  isReadOnly �Լ�

   aTrans [IN] - Read Only ����  �˻���  Transaction ��ü.
*/
idBool smxTrans::isReadOnly4Callback(void * aTrans)
{

    smxTrans * sTrans = (smxTrans*) aTrans;
    return sTrans->isReadOnly();
}



// PROJ-1362 QP-Large Record & Internal LOB
// memory lob cursor open function.
IDE_RC  smxTrans::openLobCursor(idvSQL            * aStatistics,
                                void              * aTable,
                                smiLobCursorMode    aOpenMode,
                                smSCN               aLobViewSCN,
                                smSCN               aInfinite,
                                void              * aRow,
                                const smiColumn   * aColumn,
                                UInt                aInfo,
                                smLobLocator      * aLobLocator)
{
    UInt            sState  = 0;
    smLobCursor   * sLobCursor;
    smcLobDesc    * sLobDesc;

    IDE_ASSERT( aRow    != NULL );
    IDE_ASSERT( aTable  != NULL );

    /* PROJ-2174 Supporting LOB in the volatile tablespace 
     * memory tablespace �Ӹ��ƴ϶� volatile tablespace�� �����ϴ�. */
    IDE_ASSERT( (SMI_TABLE_TYPE_IS_MEMORY(   (smcTableHeader*)aTable ) == ID_TRUE ) ||
                (SMI_TABLE_TYPE_IS_VOLATILE( (smcTableHeader*)aTable ) == ID_TRUE ) );

    IDE_TEST_RAISE( mCurLobCursorID == UINT_MAX, overflowLobCursorID);

    /* TC/FIT/Limit/sm/smx/smxTrans_openLobCursor1_malloc.sql */
    IDU_FIT_POINT_RAISE( "smxTrans::openLobCursor1::malloc",
						  insufficient_memory );

    IDE_TEST_RAISE( mLobCursorPool.alloc((void**)&sLobCursor) != IDE_SUCCESS,
                    insufficient_memory );
    sState = 1;

    sLobCursor->mLobCursorID          = mCurLobCursorID;
    sLobCursor->mLobViewEnv.mTable    = aTable;
    sLobCursor->mLobViewEnv.mRow      = aRow;
    idlOS::memcpy( &sLobCursor->mLobViewEnv.mLobCol, aColumn, ID_SIZEOF( smiColumn ) );

    sLobCursor->mLobViewEnv.mTID      = mTransID;
    sLobCursor->mLobViewEnv.mInfinite = aInfinite;

    sLobCursor->mLobViewEnv.mSCN      = aLobViewSCN;
    sLobCursor->mLobViewEnv.mOpenMode = aOpenMode;

    sLobCursor->mLobViewEnv.mWriteOffset = 0;

    sLobCursor->mLobViewEnv.mWritePhase = SM_LOB_WRITE_PHASE_NONE;
    sLobCursor->mLobViewEnv.mWriteError = ID_FALSE;
    
    /* PROJ-2174 Supporting LOB in the volatile tablespace 
     * memory tbs�� volatile tbs�� �и��ؼ� ó���Ѵ�. */
    if ( SMI_TABLE_TYPE_IS_MEMORY( (smcTableHeader*)aTable ) )
    {
        sLobCursor->mModule = &smcLobModule;
    }
    else /* SMI_TABLE_VOLATILE */
    {
        sLobCursor->mModule = &svcLobModule;
    }
    
    sLobCursor->mInfo = aInfo;

    sLobDesc = (smcLobDesc*)( (SChar*)aRow + aColumn->offset );

    if ( (sLobDesc->flag & SM_VCDESC_MODE_MASK) == SM_VCDESC_MODE_OUT )
    {
        /* Lob Version */
        IDE_TEST_RAISE( sLobDesc->mLobVersion == ID_ULONG_MAX,
                        error_version_overflow );

        if ( sLobCursor->mLobViewEnv.mOpenMode == SMI_LOB_READ_WRITE_MODE )
        {
            sLobCursor->mLobViewEnv.mLobVersion = sLobDesc->mLobVersion + 1;
        }
        else
        {
            IDE_ERROR( sLobCursor->mLobViewEnv.mOpenMode == SMI_LOB_READ_MODE );
            
            sLobCursor->mLobViewEnv.mLobVersion = sLobDesc->mLobVersion;
        }
    }
    else
    {
        sLobCursor->mLobViewEnv.mLobVersion = 0;
    }

    // PROJ-1862 Disk In Mode LOB ���� �߰�, �޸𸮿����� ������� ����
    sLobCursor->mLobViewEnv.mLobColBuf = NULL;

    // hash�� ���.
    IDE_TEST( smuHash::insertNode( &mLobCursorHash,
                                   &(sLobCursor->mLobCursorID),
                                   sLobCursor )
              != IDE_SUCCESS );

    *aLobLocator = SMI_MAKE_LOB_LOCATOR(mTransID, mCurLobCursorID);

    //memory lob cursor list�� ���.
    mMemLCL.insert(sLobCursor);

    //for replication
    /* PROJ-2174 Supporting LOB in the volatile tablespace 
     * volatile tablespace�� replication�� �ȵȴ�. */
    if ( (sLobCursor->mLobViewEnv.mOpenMode == SMI_LOB_READ_WRITE_MODE) &&
         (smcTable::needReplicate((smcTableHeader*)sLobCursor->mLobViewEnv.mTable,
                                 this) == ID_TRUE ) )
    {
        IDE_ASSERT( SMI_TABLE_TYPE_IS_MEMORY( (smcTableHeader*)aTable ) );

        IDE_TEST( sLobCursor->mModule->mWriteLog4LobCursorOpen(
                                              aStatistics,
                                              this,
                                              *aLobLocator,
                                              &(sLobCursor->mLobViewEnv))
                  != IDE_SUCCESS ) ;
    }
    else
    {
        /* do nothing */
    }

    IDE_TEST( sLobCursor->mModule->mOpen() != IDE_SUCCESS );

    mCurLobCursorID++;

    return IDE_SUCCESS;

    IDE_EXCEPTION(overflowLobCursorID);
    {
        IDE_SET(ideSetErrorCode(smERR_ABORT_overflowLobCursorID));
    }
    IDE_EXCEPTION( insufficient_memory );
    {
        IDE_SET(ideSetErrorCode(idERR_ABORT_InsufficientMemory));
    }
    IDE_EXCEPTION( error_version_overflow )
    {
        IDE_SET( ideSetErrorCode(smERR_ABORT_INTERNAL_ARG, 
                                 "open:version overflow") );
    }
    IDE_EXCEPTION_END;
    {
        if ( sState == 1 )
        {
            IDE_PUSH();
            IDE_ASSERT( mLobCursorPool.memfree((void*)sLobCursor)
                        == IDE_SUCCESS );
            IDE_POP();
        }

    }
    return IDE_FAILURE;
}

/***********************************************************************
 * Description : disk lob cursor-open
 * Implementation :
 *
 *  aStatistics    - [IN]  �������
 *  aTable         - [IN]  LOB Column�� ��ġ�� Table�� Table Header
 *  aOpenMode      - [IN]  LOB Cursor Open Mode
 *  aLobViewSCN    - [IN]  ���� �� SCN
 *  aInfinite4Disk - [IN]  Infinite SCN
 *  aRowGRID       - [IN]  �ش� Row�� ��ġ
 *  aColumn        - [IN]  LOB Column�� Column ����
 *  aInfo          - [IN]  not null ����� QP���� �����.
 *  aLobLocator    - [OUT] Open �� LOB Cursor�� ���� LOB Locator
 **********************************************************************/
IDE_RC smxTrans::openLobCursor(idvSQL*             aStatistics,
                               void*               aTable,
                               smiLobCursorMode    aOpenMode,
                               smSCN               aLobViewSCN,
                               smSCN               aInfinite4Disk,
                               scGRID              aRowGRID,
                               smiColumn*          aColumn,
                               UInt                aInfo,
                               smLobLocator*       aLobLocator)
{
    UInt              sState = 0;
    smLobCursor     * sLobCursor;
    smLobViewEnv    * sLobViewEnv;
    sdcLobColBuffer * sLobColBuf;

    IDE_ASSERT( !SC_GRID_IS_NULL(aRowGRID) );
    IDE_ASSERT( aTable != NULL );

    //disk table�̾�� �Ѵ�.
    IDE_ASSERT( SMI_TABLE_TYPE_IS_DISK( (smcTableHeader*)aTable ) == ID_TRUE );

    IDE_TEST_RAISE( mCurLobCursorID == UINT_MAX, overflowLobCursorID);

    /*
     * alloc Lob Cursor
     */
    
    /* TC/FIT/Limit/sm/smx/smxTrans_openLobCursor2_malloc1.sql */
    IDU_FIT_POINT_RAISE( "smxTrans::openLobCursor2::malloc1",
                          insufficient_memory );

    IDE_TEST_RAISE( mLobCursorPool.alloc((void**)&sLobCursor) != IDE_SUCCESS,
                    insufficient_memory );
    sState = 1;

    /* TC/FIT/Limit/sm/smx/smxTrans_openLobCursor2_malloc2.sql */
    IDU_FIT_POINT_RAISE( "smxTrans::openLobCursor2::malloc2",
						  insufficient_memory );

    IDE_TEST_RAISE( mLobColBufPool.alloc((void**)&sLobColBuf) != IDE_SUCCESS,
                    insufficient_memory );
    sState = 2;

    sLobColBuf->mBuffer     = NULL;
    sLobColBuf->mInOutMode  = SDC_COLUMN_IN_MODE;
    sLobColBuf->mLength     = 0;
    sLobColBuf->mIsNullLob  = ID_FALSE;

    /*
     * Set Lob Cursor
     */
    
    sLobCursor->mLobCursorID = mCurLobCursorID;
    sLobCursor->mInfo        = aInfo;
    sLobCursor->mModule      = &sdcLobModule;
    
    /*
     * Set Lob View Env
     */
    
    sLobViewEnv = &(sLobCursor->mLobViewEnv);

    sdcLob::initLobViewEnv( sLobViewEnv );

    sLobViewEnv->mTable          = aTable;
    sLobViewEnv->mTID            = mTransID;
    sLobViewEnv->mSCN            = aLobViewSCN;
    sLobViewEnv->mInfinite       = aInfinite4Disk;
    sLobViewEnv->mOpenMode       = aOpenMode;
    sLobViewEnv->mLobColBuf      = (void*)sLobColBuf;
    sLobViewEnv->mWriteOffset    = 0;
    sLobViewEnv->mWritePhase     = SM_LOB_WRITE_PHASE_NONE;
    sLobViewEnv->mWriteError     = ID_FALSE;

    sLobViewEnv->mLastReadOffset  = 0;
    sLobViewEnv->mLastReadLeafNodePID = SD_NULL_PID;

    sLobViewEnv->mLastWriteOffset = 0;
    sLobViewEnv->mLastWriteLeafNodePID = SD_NULL_PID;

    SC_COPY_GRID( aRowGRID, sLobCursor->mLobViewEnv.mGRID );

    idlOS::memcpy( &sLobViewEnv->mLobCol, aColumn, ID_SIZEOF(smiColumn) );

    IDE_TEST( sdcLob::readLobColBuf( aStatistics,
                                     this,
                                     sLobViewEnv )
              != IDE_SUCCESS );

    /* set version */
    IDE_TEST( sdcLob::adjustLobVersion(sLobViewEnv) != IDE_SUCCESS );

    /*
     * hash�� ���
     */
    
    IDE_TEST( smuHash::insertNode( &mLobCursorHash,
                                   &(sLobCursor->mLobCursorID),
                                   sLobCursor )
              != IDE_SUCCESS );
    
    *aLobLocator = SMI_MAKE_LOB_LOCATOR(mTransID, mCurLobCursorID);

    /*
     * disk lob cursor list�� ���.
     */
    
    mDiskLCL.insert(sLobCursor);

    /*
     * for replication
     */
    
    if ( (sLobViewEnv->mOpenMode == SMI_LOB_READ_WRITE_MODE) &&
         (smcTable::needReplicate( (smcTableHeader*)sLobViewEnv->mTable,
                                   this) == ID_TRUE ) )
    {
        IDE_TEST( sLobCursor->mModule->mWriteLog4LobCursorOpen(
                                           aStatistics,
                                           this,
                                           *aLobLocator,
                                           sLobViewEnv ) != IDE_SUCCESS ) ;
    }

    IDE_TEST( sLobCursor->mModule->mOpen() != IDE_SUCCESS );

    mCurLobCursorID++;

    return IDE_SUCCESS;

    IDE_EXCEPTION(overflowLobCursorID);
    {
        IDE_SET(ideSetErrorCode(smERR_ABORT_overflowLobCursorID));
    }
    IDE_EXCEPTION( insufficient_memory );
    {
        IDE_SET(ideSetErrorCode(idERR_ABORT_InsufficientMemory));
    }
    IDE_EXCEPTION_END;
    {
        IDE_PUSH();

        switch( sState )
        {
            case 2:
                IDE_ASSERT( mLobColBufPool.memfree((void*)sLobColBuf)
                            == IDE_SUCCESS );
            case 1:
                IDE_ASSERT( mLobCursorPool.memfree((void*)sLobCursor)
                            == IDE_SUCCESS );
                break;
            default:
                break;
        }
        
        IDE_POP();
    }
    
    return IDE_FAILURE;
}

/***********************************************************************
 * Description : close lob cursor
 * Implementation :
 *
 *  aLobLocator - [IN] ���� LOB Cursor�� ID
 **********************************************************************/
IDE_RC smxTrans::closeLobCursor(smLobCursorID aLobCursorID)
{
    smLobCursor   * sLobCursor  = NULL;

    /* BUG-40084 */
    IDE_TEST( smuHash::findNode( &mLobCursorHash,
                                 &aLobCursorID,
                                 (void **)&sLobCursor ) != IDE_SUCCESS );

    if ( sLobCursor != NULL )
    {
        // hash���� ã��, hash���� ����.
        IDE_TEST( smuHash::deleteNode( &mLobCursorHash,
                                       &aLobCursorID,
                                       (void **)&sLobCursor )
                  != IDE_SUCCESS );

        // for Replication
        /* PROJ-2174 Supporting LOB in the volatile tablespace 
         * volatile tablespace�� replication�� �ȵȴ�. */
        if ( ( sLobCursor->mLobViewEnv.mOpenMode == SMI_LOB_READ_WRITE_MODE ) &&
             ( smcTable::needReplicate((smcTableHeader*)sLobCursor->mLobViewEnv.mTable,
                                      this ) == ID_TRUE ) )
        {
            IDE_ASSERT( ( ( (smcTableHeader *)sLobCursor->mLobViewEnv.mTable )->mFlag &
                          SMI_TABLE_TYPE_MASK )
                        != SMI_TABLE_VOLATILE );

            IDE_TEST( smrLogMgr::writeLobCursorCloseLogRec(
                                        NULL, /* idvSQL* */
                                        this,
                                        SMI_MAKE_LOB_LOCATOR(mTransID, aLobCursorID))
                      != IDE_SUCCESS ) ;
        }
        else
        {
            /* do nothing */
        }

        IDE_TEST( closeLobCursorInternal( sLobCursor ) 
                  != IDE_SUCCESS );
    }

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/***********************************************************************
 * Description : LobCursor�� ���������� �ݴ� �Լ�
 *
 *  aLobCursor  - [IN]  ���� LobCursor
 **********************************************************************/
IDE_RC smxTrans::closeLobCursorInternal(smLobCursor * aLobCursor)
{
    smSCN             sMemLobSCN;
    smSCN             sDiskLobSCN;
    sdcLobColBuffer * sLobColBuf;

    /* memory�̸�, memory lob cursor list���� ����. */
    /* PROJ-2174 Supporting LOB in the volatile tablespace
     * volatile�� memory�� �����ϰ� ó�� */
    if ( (aLobCursor->mModule == &smcLobModule) ||
         (aLobCursor->mModule == &svcLobModule) )
    {
        mMemLCL.remove(aLobCursor);
    }
    else
    {
        // disk lob cursor list���� ����.
        // DISK LOB�̾�� �Ѵ�.
        IDE_ASSERT( aLobCursor->mModule == &sdcLobModule );
        mDiskLCL.remove(aLobCursor);
    }

    // fix BUG-19687
    /* BUG-31315 [sm_resource] Change allocation disk in mode LOB buffer, 
     * from Open disk LOB cursor to prepare for write 
     * LobBuffer ���� */
    sLobColBuf = (sdcLobColBuffer*) aLobCursor->mLobViewEnv.mLobColBuf;
    if ( sLobColBuf != NULL )
    {
        IDE_TEST( sdcLob::finalLobColBuffer(sLobColBuf) != IDE_SUCCESS );

        IDE_ASSERT( mLobColBufPool.memfree((void*)sLobColBuf) == IDE_SUCCESS );
        sLobColBuf = NULL;
    }

    IDE_TEST( aLobCursor->mModule->mClose() != IDE_SUCCESS );

    // memory ����.
    IDE_TEST( mLobCursorPool.memfree((void*)aLobCursor) != IDE_SUCCESS );

    // ��� lob cursor�� �����ٸ� ,���� lob cursor id�� 0���� �Ѵ�.
    mDiskLCL.getOldestSCN(&sDiskLobSCN);
    mMemLCL.getOldestSCN(&sMemLobSCN);

    if ( (SM_SCN_IS_INFINITE(sDiskLobSCN)) && (SM_SCN_IS_INFINITE(sMemLobSCN)) )
    {
        mCurLobCursorID = 0;
    }

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}
/***********************************************************************
 * Description : LobCursorID�� �ش��ϴ� LobCursor object�� return�Ѵ�.
 * Implementation :
 *
 *  aLobLocator - [IN]  LOB Cursor ID
 *  aLobLocator - [OUT] LOB Cursor
 **********************************************************************/
IDE_RC smxTrans::getLobCursor( smLobCursorID  aLobCursorID,
                               smLobCursor**  aLobCursor )
{
    IDE_TEST( smuHash::findNode(&mLobCursorHash,
                                &aLobCursorID,
                                (void **)aLobCursor)
              != IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}


/***********************************************************************
 * Description : ��� LOB Cursor�� Close�Ѵ�.
 * Implementation :
 **********************************************************************/
IDE_RC smxTrans::closeAllLobCursors()
{
    smLobCursor          * sLobCursor;
    smSCN                  sDiskLobSCN;
    smSCN                  sMemLobSCN;
    UInt                   sState = 0;

    //fix BUG-21311
    if ( mCurLobCursorID  != 0 )
    {
        IDE_TEST( smuHash::open( &mLobCursorHash ) != IDE_SUCCESS );
        sState = 1;

        IDE_TEST( smuHash::cutNode( &mLobCursorHash,
                                    (void **)&sLobCursor ) != IDE_SUCCESS );
 
        while ( sLobCursor != NULL )
        {
            IDE_TEST( closeLobCursorInternal( sLobCursor ) != IDE_SUCCESS );
            IDE_TEST( smuHash::cutNode( &mLobCursorHash,
                                        (void **)&sLobCursor)
                     != IDE_SUCCESS );
        }

        IDE_TEST( smuHash::close(&mLobCursorHash) != IDE_SUCCESS );

        mDiskLCL.getOldestSCN(&sDiskLobSCN);
        mMemLCL.getOldestSCN(&sMemLobSCN);

        IDE_ASSERT( (SM_SCN_IS_INFINITE(sDiskLobSCN)) && (SM_SCN_IS_INFINITE(sMemLobSCN)));
        IDE_ASSERT( mCurLobCursorID == 0 );
    }
    else
    {
        // zero
    }

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;
    {
        if ( sState != 0 )
        {
            IDE_PUSH();
            IDE_ASSERT( smuHash::close( &mLobCursorHash ) == IDE_SUCCESS );
            IDE_POP();
        }
    }

    return IDE_FAILURE;
}

/***********************************************************************
 * Description : ��� LOB Cursor�� Close�Ѵ�.
 * Implementation :
 **********************************************************************/
IDE_RC smxTrans::closeAllLobCursorsWithRPLog()
{
    smLobCursor          * sLobCursor;
    smSCN                  sDiskLobSCN;
    smSCN                  sMemLobSCN;
    UInt                   sState = 0;

    //fix BUG-21311
    if ( mCurLobCursorID  != 0 )
    {
        IDE_TEST( smuHash::open( &mLobCursorHash ) != IDE_SUCCESS );
        sState = 1;

        IDE_TEST( smuHash::cutNode( &mLobCursorHash,
                                    ( void ** )&sLobCursor ) != IDE_SUCCESS );

        while( sLobCursor != NULL )
        {
            // for Replication
            /* PROJ-2174 Supporting LOB in the volatile tablespace 
             * volatile tablespace�� replication�� �ȵȴ�. */
            if ( ( sLobCursor->mLobViewEnv.mOpenMode == SMI_LOB_READ_WRITE_MODE ) &&
                 ( smcTable::needReplicate( ( smcTableHeader* )sLobCursor->mLobViewEnv.mTable,
                                            this ) == ID_TRUE ) )
            {
                IDE_ASSERT( ( ( ( smcTableHeader* )sLobCursor->mLobViewEnv.mTable )->mFlag &
                              SMI_TABLE_TYPE_MASK )
                            != SMI_TABLE_VOLATILE );

                IDE_TEST( smrLogMgr::writeLobCursorCloseLogRec(
                        NULL, /* idvSQL* */
                        this,
                        SMI_MAKE_LOB_LOCATOR( mTransID, sLobCursor->mLobCursorID ) )
                    != IDE_SUCCESS ) ;
            }
            else
            {
                /* do nothing */
            }

            IDE_TEST( closeLobCursorInternal( sLobCursor ) 
                      != IDE_SUCCESS );
            IDE_TEST( smuHash::cutNode( &mLobCursorHash,
                                        (void **)&sLobCursor )
                     != IDE_SUCCESS );
        }

        IDE_TEST( smuHash::close( &mLobCursorHash ) != IDE_SUCCESS );

        mDiskLCL.getOldestSCN( &sDiskLobSCN );
        mMemLCL.getOldestSCN( &sMemLobSCN );

        IDE_ASSERT( ( SM_SCN_IS_INFINITE( sDiskLobSCN ) ) && ( SM_SCN_IS_INFINITE( sMemLobSCN ) ) );
        IDE_ASSERT( mCurLobCursorID == 0 );
    }
    else
    {
        /* do nothing */
    }

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;
    {
        if ( sState != 0 )
        {
            IDE_PUSH();
            IDE_ASSERT( smuHash::close( &mLobCursorHash ) == IDE_SUCCESS );
            IDE_POP();
        }
    }

    return IDE_FAILURE;
}

IDE_RC smxTrans::closeAllLobCursors( UInt  aInfo )
{
    smLobCursor          * sLobCursor;
    UInt                   sState = 0;

    //fix BUG-21311
    if ( mCurLobCursorID  != 0 )
    {
        IDE_TEST( smuHash::open( &mLobCursorHash ) != IDE_SUCCESS );
        sState = 1;

        IDE_TEST( smuHash::getCurNode( &mLobCursorHash,
                                       (void **)&sLobCursor ) != IDE_SUCCESS );
        while( sLobCursor != NULL )
        {
            // BUG-40427 
            // Client���� ����ϴ� LOB Cursor�� �ƴ� Cursor�� ��� �ݴ´�.
            // aInfo�� CLIENT_TRUE �̴�.
            if ( ( sLobCursor->mInfo & aInfo ) != aInfo )
            {
                // for Replication
                /* PROJ-2174 Supporting LOB in the volatile tablespace 
                 * volatile tablespace�� replication�� �ȵȴ�. */
                if ( ( sLobCursor->mLobViewEnv.mOpenMode == SMI_LOB_READ_WRITE_MODE ) &&
                     ( smcTable::needReplicate( ( smcTableHeader* )sLobCursor->mLobViewEnv.mTable,
                                                 this ) == ID_TRUE ) )
                {
                    IDE_ASSERT( ( ( ( smcTableHeader* )sLobCursor->mLobViewEnv.mTable )->mFlag &
                                  SMI_TABLE_TYPE_MASK )
                                != SMI_TABLE_VOLATILE );

                    IDE_TEST( smrLogMgr::writeLobCursorCloseLogRec(
                                                        NULL, /* idvSQL* */
                                                        this,
                                                        SMI_MAKE_LOB_LOCATOR( mTransID, 
                                                                              sLobCursor->mLobCursorID ) )
                              != IDE_SUCCESS ) ;
                }
                else
                {
                    /* do nothing */
                }
                IDE_TEST( closeLobCursorInternal( sLobCursor )
                          != IDE_SUCCESS );

                IDE_TEST( smuHash::delCurNode( &mLobCursorHash,
                                               (void **)&sLobCursor ) != IDE_SUCCESS );
            }
            else
            {
                IDE_TEST( smuHash::getNxtNode( &mLobCursorHash,
                                               (void **)&sLobCursor ) != IDE_SUCCESS );
            }
        }

        IDE_TEST( smuHash::close(&mLobCursorHash) != IDE_SUCCESS );
    }
    else
    {
        /* do nothing */
    }

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;
    {
        if ( sState != 0 )
        {
            IDE_PUSH();
            IDE_ASSERT( smuHash::close( &mLobCursorHash ) == IDE_SUCCESS );
            IDE_POP();
        }
    }

    return IDE_FAILURE;
}

UInt smxTrans::getMemLobCursorCnt(void  *aTrans, UInt aColumnID, void *aRow)
{
    smxTrans* sTrans;

    sTrans = (smxTrans*)aTrans;

    return sTrans->mMemLCL.getLobCursorCnt(aColumnID, aRow);
}

/***********************************************************************
 * Description : ���� Transaction���� �ֱٿ� Begin�� Normal Statment��
 *               Replication�� ���� Savepoint�� �����Ǿ����� Check�ϰ�
 *               ������ �ȵǾ� �ִٸ� Replication�� ���ؼ� Savepoint��
 *               �����Ѵ�. �����Ǿ� ������ ID_TRUE, else ID_FALSE
 *
 * aTrans - [IN]  Transaction Pointer
 ***********************************************************************/
idBool smxTrans::checkAndSetImplSVPStmtDepth4Repl(void* aTrans)
{
    smxTrans * sTrans = (smxTrans*) aTrans;

    return sTrans->mSvpMgr.checkAndSetImpSVP4Repl();
}

/***********************************************************************
 * Description : Transaction log buffer�� ũ�⸦ return�Ѵ�.
 *
 * aTrans - [IN]  Transaction Pointer
 ***********************************************************************/
SInt smxTrans::getLogBufferSize(void* aTrans)
{
    smxTrans * sTrans = (smxTrans*) aTrans;

    return sTrans->mLogBufferSize;
}

/***********************************************************************
 * Description : Transaction log buffer�� ũ�⸦ Need Size �̻����� ����
 *
 * Implementation :
 *    Transaction Log Buffer Size�� Need Size ���� ũ�ų� ���� ���,
 *        nothing to do
 *    Transaction Log Buffer Size�� Need Size ���� ���� ���,
 *        log buffer Ȯ��
 *
 * aNeedSize - [IN]  Need Log Buffer Size
 *
 * Related Issue:
 *     PROJ-1665 Parallel Direct Path Insert
 *
 ***********************************************************************/
IDE_RC smxTrans::setLogBufferSize( UInt  aNeedSize )
{
    SChar * sLogBuffer;
    UInt    sState  = 0;

    if ( aNeedSize > mLogBufferSize )
    {
        mLogBufferSize = idlOS::align(aNeedSize,
                                      SMX_TRANSACTION_LOG_BUFFER_ALIGN_SIZE);

        sLogBuffer = NULL;

        IDE_TEST( iduMemMgr::malloc(IDU_MEM_SM_TRANSACTION_TABLE,
                                    mLogBufferSize,
                                    (void**)&sLogBuffer,
                                    IDU_MEM_FORCE)
                  != IDE_SUCCESS );
        sState = 1;

        if ( mLogOffset != 0 )
        {
            idlOS::memcpy( sLogBuffer, mLogBuffer, mLogOffset );
        }
        else
        {
            /* nothing to do */
        }

        IDE_TEST( iduMemMgr::free(mLogBuffer) != IDE_SUCCESS );

        mLogBuffer = sLogBuffer;

        // ���� �α׹��۸� �̿��Ͽ�  ����Ǵ� �α��� ũ���� ������
        // �̹� �������ֱ� ������ ����α� ������ ũ��� ������ �ʿ䰡 ����.
    }

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    switch( sState )
    {
        case 1:
            IDE_ASSERT( iduMemMgr::free( sLogBuffer ) == IDE_SUCCESS );
            sLogBuffer = NULL;
        default:
            break;
    }

    return IDE_FAILURE;
}

/***********************************************************************
 * Description : Transaction log buffer�� ũ�⸦ Need Size �̻����� ����
 *
 * aTrans    - [IN]  Transaction Pointer
 * aNeedSize - [IN]  Need Log Buffer Size
 *
 * Related Issue:
 *     PROJ-1665 Parallel Direct Path Insert
 *
 ***********************************************************************/
IDE_RC smxTrans::setTransLogBufferSize( void * aTrans,
                                        UInt   aNeedSize )
{
   smxTrans * sTrans = (smxTrans*) aTrans;

   return sTrans->setLogBufferSize( aNeedSize );
}

void smxTrans::setFreeInsUndoSegFlag( void   * aTrans,
                                      idBool   aFlag )
{
    IDE_DASSERT( aTrans != NULL );

    ((smxTrans*)aTrans)->mFreeInsUndoSegFlag = aFlag;

    return;
}

void smxTrans::setMemoryTBSAccessed4Callback(void * aTrans)
{
    smxTrans * sTrans;

    IDE_DASSERT( aTrans != NULL );

    sTrans = (smxTrans*) aTrans;

    sTrans->setMemoryTBSAccessed();
}

/***********************************************************************
 * Description : ���� ������ Prepare Ʈ������� Ʈ����� ���׸�Ʈ ���� ����
 ***********************************************************************/
void smxTrans::setXaSegsInfo( void    * aTrans,
                              UInt      aTxSegEntryIdx,
                              sdRID     aExtRID4TSS,
                              scPageID  aFstPIDOfLstExt4TSS,
                              sdRID     aFstExtRID4UDS,
                              sdRID     aLstExtRID4UDS,
                              scPageID  aFstPIDOfLstExt4UDS,
                              scPageID  aFstUndoPID,
                              scPageID  aLstUndoPID )
{
    sdcTXSegEntry * sTXSegEntry;

    IDE_ASSERT( aTrans != NULL );

    sTXSegEntry = ((smxTrans*) aTrans)->mTXSegEntry;

    IDE_ASSERT( sTXSegEntry != NULL );

    ((smxTrans*)aTrans)->mTXSegEntryIdx = aTxSegEntryIdx;
    sTXSegEntry->mExtRID4TSS            = aExtRID4TSS;

    sTXSegEntry->mTSSegmt.setCurAllocInfo( aExtRID4TSS,
                                           aFstPIDOfLstExt4TSS,
                                           SD_MAKE_PID(sTXSegEntry->mTSSlotSID) );

    if ( sTXSegEntry->mFstExtRID4UDS == SD_NULL_RID )
    {
        IDE_ASSERT( sTXSegEntry->mLstExtRID4UDS == SD_NULL_RID );
    }

    if ( sTXSegEntry->mFstUndoPID == SD_NULL_PID )
    {
        IDE_ASSERT( sTXSegEntry->mLstUndoPID == SD_NULL_PID );
    }

    sTXSegEntry->mFstExtRID4UDS = aFstExtRID4UDS;
    sTXSegEntry->mLstExtRID4UDS = aLstExtRID4UDS;
    sTXSegEntry->mFstUndoPID    = aFstUndoPID;
    sTXSegEntry->mLstUndoPID    = aLstUndoPID;

    sTXSegEntry->mUDSegmt.setCurAllocInfo( aLstExtRID4UDS,
                                           aFstPIDOfLstExt4UDS,
                                           aLstUndoPID );
}


/* TableInfo�� �˻��Ͽ� HintDataPID�� ��ȯ�Ѵ�. */
void smxTrans::getHintDataPIDofTableInfo( void       *aTableInfo,
                                          scPageID   *aHintDataPID )
{
    smxTableInfo *sTableInfoPtr = (smxTableInfo*)aTableInfo;

    if ( sTableInfoPtr != NULL )
    {
        smxTableInfoMgr::getHintDataPID(sTableInfoPtr, aHintDataPID );
    }
    else
    {
        *aHintDataPID = SD_NULL_PID;
    }
}

/* TableInfo�� �˻��Ͽ� HintDataPID�� �����Ѵ�.. */
void smxTrans::setHintDataPIDofTableInfo( void       *aTableInfo,
                                          scPageID    aHintDataPID )
{
    smxTableInfo *sTableInfoPtr = (smxTableInfo*)aTableInfo;

    if (sTableInfoPtr != NULL )
    {
        smxTableInfoMgr::setHintDataPID( sTableInfoPtr, aHintDataPID );
    }
}

idBool smxTrans::isNeedLogFlushAtCommitAPrepare( void * aTrans )
{
    smxTrans * sTrans = (smxTrans*)aTrans;
    return sTrans->isNeedLogFlushAtCommitAPrepareInternal();
}

/*******************************************************************************
 * Description : DDL Transaction���� ��Ÿ���� Log Record�� ����Ѵ�.
 ******************************************************************************/
IDE_RC smxTrans::writeDDLLog()
{
    smrDDLLog  sLogHeader;
    smrLogType sLogType = SMR_LT_DDL;

    initLogBuffer();

    /* Log header�� �����Ѵ�. */
    idlOS::memset(&sLogHeader, 0, ID_SIZEOF(smrDDLLog));

    smrLogHeadI::setType(&sLogHeader.mHead, sLogType);

    smrLogHeadI::setSize(&sLogHeader.mHead,
                         SMR_LOGREC_SIZE(smrDDLLog) +
                         ID_SIZEOF(smrLogTail));

    smrLogHeadI::setTransID(&sLogHeader.mHead, mTransID);

    smrLogHeadI::setPrevLSN(&sLogHeader.mHead, mLstUndoNxtLSN);

    // BUG-23045 [RP] SMR_LT_DDL�� Log Type Flag��
    //           Transaction Begin���� ������ ���� ����ؾ� �մϴ�
    smrLogHeadI::setFlag(&sLogHeader.mHead, mLogTypeFlag);

    /* BUG-24866
     * [valgrind] SMR_SMC_PERS_WRITE_LOB_PIECE �α׿� ���ؼ�
     * Implicit Savepoint�� �����ϴµ�, mReplSvPNumber�� �����ؾ� �մϴ�. */
    smrLogHeadI::setReplStmtDepth( &sLogHeader.mHead,
                                   SMI_STATEMENT_DEPTH_NULL );

    /* Write log header */
    IDE_TEST( writeLogToBuffer( (const void *)&sLogHeader,
                                SMR_LOGREC_SIZE(smrDDLLog) )
             != IDE_SUCCESS );

    /* Write log tail */
    IDE_TEST( writeLogToBuffer( (const void *)&sLogType,
                                ID_SIZEOF(smrLogType) )
             != IDE_SUCCESS );

    IDE_TEST( writeTransLog(this) != IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

IDE_RC smxTrans::addTouchedPage( scSpaceID   aSpaceID,
                                 scPageID    aPageID,
                                 SShort      aCTSlotNum )
{
    /* BUG-34446 DPath INSERT�� �����Ҷ��� TPH�� �����ϸ� �ȵ˴ϴ�. */
    if ( mDPathEntry == NULL )
    {
        IDE_TEST( mTouchPageList.add( aSpaceID,
                                      aPageID,
                                      aCTSlotNum )
                  != IDE_SUCCESS );
    }

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*******************************************************************************
 * Description : Staticstics�� ��Ʈ�Ѵ�.
 *
 *  BUG-22651  smrLogMgr::updateTransLSNInfo����
 *             ����������Ǵ� ��찡 �����ֽ��ϴ�.
 ******************************************************************************/
void smxTrans::setStatistics( idvSQL * aStatistics )
{
    mStatistics = aStatistics;
    //fix BUG-23656 session,xid ,transaction�� ������ performance view�� �����ϰ�,
    //�׵鰣�� ���踦 ��Ȯ�� �����ؾ� ��.
    // transaction�� ����ϴ� session�� ����.
    if ( aStatistics != NULL )
    {
        if ( aStatistics->mSess != NULL )
        {
            mSessionID = aStatistics->mSess->mSID;
        }
        else
        {
            mSessionID = ID_NULL_SESSION_ID;
        }
    }
    else
    {
        mSessionID = ID_NULL_SESSION_ID;
    }
}

/***********************************************************************
 *
 * Description :
 *  infinite scn���� ������Ű��,
 *  output parameter�� ������ infinite scn���� ��ȯ�Ѵ�.
 *
 *  aSCN    - [OUT] ������ infinite scn ��
 *
 **********************************************************************/
IDE_RC smxTrans::incInfiniteSCNAndGet(smSCN *aSCN)
{
    smSCN sTempScn = mInfinite;

    SM_ADD_INF_SCN( &sTempScn );
    IDE_TEST_RAISE( SM_SCN_IS_LT(&sTempScn, &mInfinite) == ID_TRUE,
                    ERR_OVERFLOW );

    SM_ADD_INF_SCN( &mInfinite );

    *aSCN = mInfinite;
    return IDE_SUCCESS;

    IDE_EXCEPTION( ERR_OVERFLOW );
    IDE_SET( ideSetErrorCode( smERR_ABORT_smiUpdateOverflow ) );
    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

ULong smxTrans::getLockTimeoutByUSec( )
{
    return getLockTimeoutByUSec ( smuProperty::getLockTimeOut() );
}
/*
 * BUG-20589 Receiver�� REPLICATION_LOCK_TIMEOUT ����
 * BUG-33539
 * receiver���� lock escalation�� �߻��ϸ� receiver�� self deadlock ���°� �˴ϴ�
 */
ULong smxTrans::getLockTimeoutByUSec( ULong aLockWaitMicroSec )
{
    ULong sLockTimeOut;

    if ( isReplTrans() == ID_TRUE )
    {
        sLockTimeOut = mReplLockTimeout * 1000000;
    }
    else
    {
        sLockTimeOut = aLockWaitMicroSec;
    }

    return sLockTimeOut;
}

IDE_RC smxTrans::setReplLockTimeout( UInt aReplLockTimeout )
{
    IDE_TEST_RAISE( ( mStatus == SMX_TX_END ) || ( isReplTrans() != ID_TRUE ), ERR_SET_LOCKTIMEOUT );

    mReplLockTimeout = aReplLockTimeout;

    return IDE_SUCCESS;

    IDE_EXCEPTION( ERR_SET_LOCKTIMEOUT )
    {
        IDE_SET( ideSetErrorCode( smERR_ABORT_INTERNAL ) );
    }
    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}
/***********************************************************************
 * Description : BUG-43595 �� ���Ͽ� transaction �ɹ� ���� ���
 *
 **********************************************************************/
void smxTrans::dumpTransInfo()
{
    ideLog::log(IDE_ERR_0,
                "Dump Transaction Info [%"ID_UINT32_FMT"] %"ID_UINT32_FMT"\n"
                "TransID         : %"ID_UINT32_FMT"\n"
                "mSlotN          : %"ID_UINT32_FMT"\n"
                "Flag            : %"ID_UINT32_FMT"\n"
                "SessionID       : %"ID_UINT32_FMT"\n"
                "MinMemViewSCN   : 0x%"ID_XINT64_FMT"\n"
                "MinDskViewSCN   : 0x%"ID_XINT64_FMT"\n"
                "FstDskViewSCN   : 0x%"ID_XINT64_FMT"\n"
                "OldestFstViewSCN: 0x%"ID_XINT64_FMT"\n"
                "CommitSCN       : 0x%"ID_XINT64_FMT"\n"
                "Infinite        : 0x%"ID_XINT64_FMT"\n"
                "Status          : 0x%"ID_XINT32_FMT"\n"
                "Status4FT       : 0x%"ID_XINT32_FMT"\n"
                "LSLockFlag      : %"ID_UINT32_FMT"\n"
                "IsUpdate        : %"ID_UINT32_FMT"\n"
                "IsTransWaitRepl : %"ID_UINT32_FMT"\n"
                "IsFree          : %"ID_UINT32_FMT"\n"
                "ReplID          : %"ID_UINT32_FMT"\n"
                "IsWriteImpLog   : %"ID_UINT32_FMT"\n"
                "LogTypeFlag     : %"ID_UINT32_FMT"\n"
                "CommitState     : %"ID_XINT32_FMT"\n"
                "TransFreeList   : 0x%"ID_XPOINTER_FMT"\n"
                "FstUndoNxtLSN   : ( %"ID_INT32_FMT", %"ID_INT32_FMT" )\n"
                "LstUndoNxtLSN   : ( %"ID_INT32_FMT", %"ID_INT32_FMT" )\n"
                "TotalLogCount   : %"ID_UINT32_FMT"\n"
                "ProcessedUndoLogCount : %"ID_UINT32_FMT"\n"
                "UndoBeginTime   : %"ID_UINT32_FMT"\n"
                "UpdateSize      : %"ID_UINT64_FMT"\n"
                "AbleToRollback  : %"ID_UINT32_FMT"\n"
                "FstUpdateTime   : %"ID_UINT32_FMT"\n"
                "LogBufferSize   : %"ID_UINT32_FMT"\n"
                "LogOffset       : %"ID_UINT32_FMT"\n"
                "DoSkipCheck     : %"ID_UINT32_FMT"\n"
                "DoSkipCheckSCN  : %"ID_XINT64_FMT"\n"
                "IsDDL           : %"ID_UINT32_FMT"\n"
                "IsFirstLog      : %"ID_UINT32_FMT"\n"
                "LegacyTransCnt  : %"ID_UINT32_FMT"\n"
                "TXSegEntryIdx   : %"ID_INT32_FMT"\n"
                "CurUndoNxtLSN   : ( %"ID_INT32_FMT", %"ID_INT32_FMT" )\n"
                "LastWritedLSN   : ( %"ID_INT32_FMT", %"ID_INT32_FMT" )\n"
                "BeginLogLSN     : ( %"ID_INT32_FMT", %"ID_INT32_FMT" )\n"
                "CommitLogLSN    : ( %"ID_INT32_FMT", %"ID_INT32_FMT" )\n"
                "RSGroupID       : %"ID_INT32_FMT"\n"
                "CurLobCursorID  : %"ID_UINT32_FMT"\n"
                "FreeInsUndoSegFlag : %"ID_UINT32_FMT"\n"
                "DiskTBSAccessed    : %"ID_UINT32_FMT"\n"
                "MemoryTBSAccessed  : %"ID_UINT32_FMT"\n"
                "MetaTableModified  : %"ID_UINT32_FMT"\n",
                mSlotN,
                mTransID,
                mTransID,
                mSlotN,
                mFlag,
                mSessionID,
                mMinMemViewSCN,
                mMinDskViewSCN,
                mFstDskViewSCN,
                mOldestFstViewSCN,
                mCommitSCN,
                mInfinite,
                mStatus,
                mStatus4FT,
                mLSLockFlag,
                mIsUpdate,
                mIsTransWaitRepl,
                mIsFree,
                mReplID,
                mIsWriteImpLog,
                mLogTypeFlag,
                mCommitState,
                mTransFreeList, // pointer
                mFstUndoNxtLSN.mFileNo,
                mFstUndoNxtLSN.mOffset,
                mLstUndoNxtLSN.mFileNo,
                mLstUndoNxtLSN.mOffset,
                mTotalLogCount,
                mProcessedUndoLogCount,
                mUndoBeginTime,
                mUpdateSize,
                mAbleToRollback,
                mFstUpdateTime,
                mLogBufferSize,
                mLogOffset,
                mDoSkipCheck,
                mDoSkipCheckSCN,
                mIsDDL,
                mIsFirstLog,
                mLegacyTransCnt,
                mTXSegEntryIdx,
                mCurUndoNxtLSN.mFileNo,
                mCurUndoNxtLSN.mOffset,
                mLastWritedLSN.mFileNo,
                mLastWritedLSN.mOffset,
                mBeginLogLSN.mFileNo,
                mBeginLogLSN.mOffset,
                mCommitLogLSN.mFileNo,
                mCommitLogLSN.mOffset,
                mRSGroupID,
                mCurLobCursorID,
                mFreeInsUndoSegFlag,
                mDiskTBSAccessed,
                mMemoryTBSAccessed,
                mMetaTableModified );
}