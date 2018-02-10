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
 * $Id: rpdQueue.cpp 82075 2018-01-17 06:39:52Z jina.kim $
 **********************************************************************/

#include <rpdQueue.h>

IDE_RC rpdQueue::initialize(SChar *aRepName)
{
    SChar    sName[IDU_MUTEX_NAME_LEN + 1];

    mHeadPtr = NULL;
    mTailPtr = NULL;
    mXLogCnt = 0;
    mWaitFlag = ID_FALSE;
    mFinalFlag = ID_FALSE;

    idlOS::memset(&mMutex, 0, ID_SIZEOF(iduMutex));
    idlOS::snprintf(sName, IDU_MUTEX_NAME_LEN, "RP_%s_RECV_QUEUE_MUTEX",
                    aRepName);
    /*
     * BUG-34125 Posix mutex must be used for cond_timedwait(), cond_wait().
     */
    IDE_TEST_RAISE(mMutex.initialize((SChar *)sName,
                                     IDU_MUTEX_KIND_POSIX,
                                     IDV_WAIT_INDEX_NULL)
                   != IDE_SUCCESS, ERR_MUTEX_INIT);

    IDE_TEST_RAISE(mXLogQueueCV.initialize() != IDE_SUCCESS,
                   ERR_COND_INIT);

    return IDE_SUCCESS;

    IDE_EXCEPTION(ERR_MUTEX_INIT);
    {
        IDE_ERRLOG(IDE_RP_0);
        IDE_SET(ideSetErrorCode(rpERR_FATAL_ThrMutexInit));
        IDE_ERRLOG(IDE_RP_0);

        IDE_CALLBACK_FATAL("[Repl] Mutex initialization error");
    }
    IDE_EXCEPTION(ERR_COND_INIT);
    {
        IDE_SET(ideSetErrorCode(rpERR_FATAL_ThrCondInit));
        IDE_ERRLOG(IDE_RP_0);

        IDE_CALLBACK_FATAL("[Repl] idlOS::cond_init() error");
    }
    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

void rpdQueue::finalize()
{
    IDE_ASSERT(lock() == IDE_SUCCESS);

    mFinalFlag = ID_TRUE;
    IDE_ASSERT(mXLogQueueCV.signal() == IDE_SUCCESS);

    IDE_ASSERT(unlock() == IDE_SUCCESS);

    return;
}

void rpdQueue::write(rpdXLog *aXLogPtr)
{
    IDE_ASSERT(lock() == IDE_SUCCESS);

    if(mHeadPtr == NULL)
    {
        // First Entry
        mHeadPtr = aXLogPtr;
        mTailPtr = aXLogPtr;
        aXLogPtr->mNext = NULL;
    }
    else
    {
        // Next Entry
        mTailPtr->mNext = aXLogPtr;
        mTailPtr = aXLogPtr;
        aXLogPtr->mNext = NULL;
    }

    mXLogCnt ++;
    if(mWaitFlag != ID_FALSE)
    {
        IDE_ASSERT(mXLogQueueCV.signal() == IDE_SUCCESS);
    }
    IDE_ASSERT(unlock() == IDE_SUCCESS);

    return;
}

//call by destroy
void rpdQueue::read(rpdXLog **aXLogPtr)
{
    IDE_ASSERT(lock() == IDE_SUCCESS);

    /* Get First Entry */
    *aXLogPtr = mHeadPtr;

    if(mHeadPtr != NULL)
    {
        mHeadPtr = mHeadPtr->mNext;
        mXLogCnt --;
    }

    if(mHeadPtr == NULL)
    {
        mTailPtr = NULL;
    }

    IDE_ASSERT(unlock() == IDE_SUCCESS);

    return;
}
/****************************************************
* call by rpxReceiverApply recvXLog
* final�Ǵ� ��쿡 *aXLogPtr�� NULL�� set�ȴ�. 
****************************************************/
void rpdQueue::read(rpdXLog **aXLogPtr, smSN *aTailLogSN)
{
    IDE_ASSERT(lock() == IDE_SUCCESS);

    //Queue�� �� ��� ���
    while((mXLogCnt == 0) && (mFinalFlag != ID_TRUE))
    {
        mWaitFlag = ID_TRUE;
        (void)mXLogQueueCV.wait(&mMutex);
        mWaitFlag = ID_FALSE;
    }

    /* Get First Entry */
    *aXLogPtr = mHeadPtr;

    if(mHeadPtr != NULL)
    {
        mHeadPtr = mHeadPtr->mNext;
        mXLogCnt --;
    }

    if(mHeadPtr == NULL)
    {
        mTailPtr = NULL;
    }

    if(aTailLogSN != NULL)
    {
        if(mTailPtr != NULL)
        {
            *aTailLogSN = mTailPtr->mSN;
        }
        else
        {
            *aTailLogSN = SM_SN_NULL;
        }
    }

    IDE_ASSERT(unlock() == IDE_SUCCESS);

    return;
}

IDE_RC rpdQueue::allocXLog(rpdXLog **aXLogPtr, iduMemAllocator * aAllocator )
{
    *aXLogPtr = NULL;

    IDE_TEST_RAISE( iduMemMgr::malloc( IDU_MEM_RP_RPD,
                                       ID_SIZEOF(rpdXLog),
                                       (void **)aXLogPtr,
                                       IDU_MEM_IMMEDIATE,
                                       aAllocator )
                    != IDE_SUCCESS, ERR_MEMORY_ALLOC_XLOG);
    (void)idlOS::memset(*aXLogPtr, 0, ID_SIZEOF(rpdXLog));

    return IDE_SUCCESS;

    IDE_EXCEPTION(ERR_MEMORY_ALLOC_XLOG);
    {
        IDE_ERRLOG(IDE_RP_0);
        IDE_SET(ideSetErrorCode(rpERR_ABORT_MEMORY_ALLOC,
                                "rpdQueue::allocXLog",
                                "aXLogPtr"));
    }
    IDE_EXCEPTION_END;
    IDE_PUSH();

    if(*aXLogPtr != NULL)
    {
        (void)iduMemMgr::free(*aXLogPtr, aAllocator);
        *aXLogPtr = NULL;
    }

    IDE_POP();
    return IDE_FAILURE;
}

void rpdQueue::freeXLog(rpdXLog *aXLogPtr, iduMemAllocator * aAllocator )
{
    destroyXLog( aXLogPtr, aAllocator );

    if ( aXLogPtr != NULL )
    {
        (void)iduMemMgr::free( aXLogPtr, aAllocator );
    }

    return;
}

/**
* @breif XLog�� �ʱ�ȭ�Ѵ�.
*
* @param aXLogPtr �ʱ�ȭ�� XLog
* @param aBufferSize smiValue->value�� �Ҵ��� �޸𸮸� �����ϴ� ������ �⺻ ũ��
*/
IDE_RC rpdQueue::initializeXLog( rpdXLog         * aXLogPtr,
                                 ULong             aBufferSize,
                                 idBool            aIsLobExist,
                                 iduMemAllocator * aAllocator )
{
    idBool sIsMemInit = ID_FALSE;
    idBool sIsAllocLob = ID_FALSE;

    /* rpsSmExecutor���� ����ϴ� �ʼ����� �κ� */
    aXLogPtr->mLobPtr = NULL;

    IDE_TEST( aXLogPtr->mMemory.init( IDU_MEM_RP_RPD, aBufferSize )
              != IDE_SUCCESS );
    sIsMemInit = ID_TRUE;

    if ( aIsLobExist == ID_TRUE )
    {
        IDE_TEST_RAISE( iduMemMgr::malloc( IDU_MEM_RP_RPD,
                                           ID_SIZEOF(rpdLob),
                                           (void **)&(aXLogPtr->mLobPtr),
                                           IDU_MEM_IMMEDIATE,
                                           aAllocator )
                        != IDE_SUCCESS, ERR_MEMORY_ALLOC_XLOG );
        sIsAllocLob = ID_TRUE;

		aXLogPtr->mLobPtr->mLobPiece = NULL;
    }
    else
    {
        /* nothing to do */
    }

    recycleXLog( aXLogPtr, aAllocator );

    return IDE_SUCCESS;

    IDE_EXCEPTION(ERR_MEMORY_ALLOC_XLOG);
    {
        IDE_ERRLOG(IDE_RP_0);
        IDE_SET(ideSetErrorCode(rpERR_ABORT_MEMORY_ALLOC,
                                "rpdQueue::initializeXLog",
                                "aXLogPtr->mLobPtr"));
    }
    IDE_EXCEPTION_END;

    if ( sIsAllocLob == ID_TRUE )
    {
        sIsAllocLob = ID_FALSE;
        iduMemMgr::free( aXLogPtr->mLobPtr, aAllocator ); 
		aXLogPtr->mLobPtr = NULL;
    }
    else
    {
        /* nothing to do */
    }

    if ( sIsMemInit == ID_TRUE )
    {
        sIsMemInit = ID_FALSE;
        aXLogPtr->mMemory.freeAll( 0 );
        aXLogPtr->mMemory.destroy();
    }
    else
    {
        /* nothing to do */
    }

    return IDE_FAILURE;
}

/**
 * @breif XLog�� �Ҵ�� �ڿ��� �ݳ��Ѵ�.
 *
 * @param aXLogPtr �ڿ��� �ݳ��� XLog
 */
void rpdQueue::destroyXLog( rpdXLog * aXLogPtr, iduMemAllocator * aAllocator )
{
    aXLogPtr->mMemory.freeAll( 0 );
    aXLogPtr->mMemory.destroy();

    if ( aXLogPtr->mLobPtr != NULL )
    {
        if ( aXLogPtr->mLobPtr->mLobPiece != NULL )
        {
            (void)iduMemMgr::free((void *)aXLogPtr->mLobPtr->mLobPiece, aAllocator);
            aXLogPtr->mLobPtr->mLobPiece = NULL;        
        }
        else
        {
            /* nothing to do */
        }

        (void)iduMemMgr::free((void *)aXLogPtr->mLobPtr, aAllocator);
        aXLogPtr->mLobPtr = NULL;    
    }

    return;
}

/**
 * @breif XLog�� �Ҵ�� �ڿ��� ������ �� �ְ� �Ѵ�.
 *
 * @param aXLogPtr �ڿ��� ������ XLog
 */
void rpdQueue::recycleXLog( rpdXLog * aXLogPtr, iduMemAllocator * aAllocator )
{
    aXLogPtr->mMemory.freeAll( 1 );

    if(aXLogPtr->mLobPtr != NULL)
    {
        /* mLobPtr �� ��Ȱ�� �Ѵ�. */
        if ( aXLogPtr->mLobPtr->mLobPiece != NULL )
        {
            (void)iduMemMgr::free((void *)aXLogPtr->mLobPtr->mLobPiece, aAllocator);
            aXLogPtr->mLobPtr->mLobPiece = NULL;
        }
        else
        {
            /* nothing to do */
        }
    }

    aXLogPtr->mCIDs = NULL;
    aXLogPtr->mBCols = NULL;
    aXLogPtr->mACols = NULL;
    aXLogPtr->mPKCols = NULL;
    aXLogPtr->mSPName = NULL;

    aXLogPtr->mPKColCnt = 0;
    aXLogPtr->mColCnt = 0;
    aXLogPtr->mSPNameLen = 0;
    
    /*SN�� ���۵��� �ʴ� ��찡 �־� �ʱ�ȭ �ؾ���.*/
    aXLogPtr->mSN = SM_SN_NULL;
    aXLogPtr->mRestartSN = SM_SN_NULL;

    return;
}

UInt rpdQueue::getSize()
{
    UInt sSize = 0;
    IDE_ASSERT(lock() == IDE_SUCCESS);
    sSize = mXLogCnt;
    IDE_ASSERT(unlock() == IDE_SUCCESS);
    return sSize;
}

void rpdQueue::setWaitCondition( rpdXLog    * aXLog,
                                 UInt         aLastWaitApplierIndex,
                                 smSN         aLastWaitSN )
{
    aXLog->mWaitSNFromOtherApplier = aLastWaitSN;
    aXLog->mWaitApplierIndex = aLastWaitApplierIndex;
}