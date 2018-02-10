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
 * $Id: sdpTableSpace.cpp 82075 2018-01-17 06:39:52Z jina.kim $
 **********************************************************************/

#include <sdd.h>
#include <sdb.h>
#include <sct.h>
#include <sdpModule.h>
#include <sdpTableSpace.h>
#include <sdpReq.h>
#include <smErrorCode.h>
#include <sdptbExtent.h>

/*
 * ��� ��ũ ���̺������̽��� Space Cache �Ҵ� �� �ʱ�ȭ�Ѵ�.
 */
IDE_RC sdpTableSpace::initialize()
{
    // Space Cache�� �Ҵ��ϰ� �ʱ�ȭ�Ѵ�.
    IDE_TEST( sctTableSpaceMgr::doAction4EachTBS(
                  NULL, /* aStatistics */
                  doActAllocSpaceCache,
                  NULL, /* Action Argument*/
                  SCT_ACT_MODE_NONE )
              != IDE_SUCCESS );

    // BUG-24434
    // sdpPageType �� ������ �Ǹ� IDV_SM_PAGE_TYPE_MAX ���� Ȯ���� ��� �մϴ�.  
    IDE_ASSERT( IDV_SM_PAGE_TYPE_MAX == SDP_PAGE_TYPE_MAX );

    return IDE_SUCCESS ;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*
 * ��� ��ũ ���̺������̽��� Space Cache��
 * �޸� �����Ѵ�.
 */
IDE_RC sdpTableSpace::destroy()
{
    IDE_TEST( sctTableSpaceMgr::doAction4EachTBS(
                  NULL, /* aStatistics */
                  doActFreeSpaceCache,
                  NULL, /* Action Argument*/
                  SCT_ACT_MODE_NONE )
              != IDE_SUCCESS );

    return IDE_SUCCESS ;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/***********************************************************************
 * Description : Disk Tablespace�� Create�Ѵ�.
 **********************************************************************/
IDE_RC sdpTableSpace::createTBS( idvSQL            * aStatistics,
                                 smiTableSpaceAttr * aTableSpaceAttr,
                                 smiDataFileAttr  ** aDataFileAttr,
                                 UInt                aDataFileAttrCount,
                                 void*               aTrans )
{
    sdrMtxStartInfo   sStartInfo;
    sdpExtMgmtOp    * sTBSMgrOp;

    IDE_DASSERT( aTableSpaceAttr    != NULL );
    IDE_DASSERT( aDataFileAttr      != NULL );
    IDE_DASSERT( aDataFileAttrCount != 0 );
    IDE_DASSERT( aTrans             != NULL );

    sTBSMgrOp  = getTBSMgmtOP( aTableSpaceAttr->mDiskAttr.mExtMgmtType );

    sStartInfo.mTrans = aTrans;
    sStartInfo.mLogMode = SDR_MTX_LOGGING;

    /* FIT/ART/sm/Design/Resource/Bugs/BUG-14900/BUG-14900.tc */
    IDU_FIT_POINT( "1.TASK-1842@sdpTableSpace::createTBS" );
    IDE_TEST( sTBSMgrOp->mCreateTBS( aStatistics,
                                     &sStartInfo,
                                     aTableSpaceAttr,
                                     aDataFileAttr,
                                     aDataFileAttrCount ) != IDE_SUCCESS );


    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*
 * Tablespace ����
 */
IDE_RC sdpTableSpace::dropTBS( idvSQL       *aStatistics,
                               void*         aTrans,
                               scSpaceID     aSpaceID,
                               smiTouchMode  aTouchMode )
{
    sdpExtMgmtOp * sTBSMgrOp;

    sTBSMgrOp  = getTBSMgmtOP( aSpaceID );

    IDE_TEST( sTBSMgrOp->mDropTBS( aStatistics,
                                   aTrans,
                                   aSpaceID,
                                   aTouchMode ) != IDE_SUCCESS );

    /* FIT/ART/sm/Design/Resource/TASK-1842/DROP_TABLESPACE.tc */
    IDU_FIT_POINT( "1.PROJ-1548@sdpTableSpace::dropTBS" );


    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*
 * Tablespace ����
 */
IDE_RC sdpTableSpace::resetTBS( idvSQL           *aStatistics,
                                scSpaceID         aSpaceID,
                                void             *aTrans )
{
    sdpExtMgmtOp        * sTBSMgrOp;
    sddTableSpaceNode   * sSpaceNode;

    sTBSMgrOp = getTBSMgmtOP( aSpaceID );

    IDE_TEST( sTBSMgrOp->mResetTBS( aStatistics,
                                    aTrans,
                                    aSpaceID )
              != IDE_SUCCESS );

    IDE_ASSERT( sctTableSpaceMgr::findSpaceNodeBySpaceID( aSpaceID,
                                                          (void**)&sSpaceNode )

              == IDE_SUCCESS );

    if( sctTableSpaceMgr::isTempTableSpace( aSpaceID ) == ID_TRUE )
    {
        if( sTBSMgrOp->mRefineSpaceCache != NULL )
        {
            IDE_TEST( sTBSMgrOp->mRefineSpaceCache( sSpaceNode )
                      != IDE_SUCCESS );
        }
    }

    return IDE_SUCCESS ;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}


/***********************************************************************
 * Description : TableSpace�� extent ���� ���� ����� �����Ѵ�.
 *
 *  aSpaceID - [IN] Ȯ���ϰ��� �ϴ� spage�� id
 **********************************************************************/
smiExtMgmtType sdpTableSpace::getExtMgmtType( scSpaceID   aSpaceID )
{
    sdpSpaceCacheCommon * sCacheHeader;
    smiExtMgmtType        sExtMgmtType;

    if ( sctTableSpaceMgr::hasState( aSpaceID, SCT_SS_INVALID_DISK_TBS )
         == ID_FALSE )
    {
        sCacheHeader =
            (sdpSpaceCacheCommon*) sddDiskMgr::getSpaceCache( aSpaceID );

        IDE_ASSERT( sCacheHeader != NULL );

        sExtMgmtType = sCacheHeader->mExtMgmtType;
    }
    else
    {
        sExtMgmtType = SMI_EXTENT_MGMT_NULL_TYPE;
    }

    return sExtMgmtType;
}


/*
 * Segment �������� ����� Tablespace �������� ����� ������.
 * ���� ���� �ٸ� ����� �߱��� ���� ������, ����ν��
 * ���� ������ �Ѱ������� ���ؼ� Tablespace �������������
 * �������� Segment �������� ��ĵ� ���������� ó���Ѵ�.
 */
smiSegMgmtType sdpTableSpace::getSegMgmtType( scSpaceID   aSpaceID )
{
    sdpSpaceCacheCommon * sCacheHeader;
    smiSegMgmtType        sSegMgmtType;

    if ( sctTableSpaceMgr::hasState( aSpaceID, SCT_SS_INVALID_DISK_TBS )
         == ID_FALSE )
    {
        if ( aSpaceID == SMI_ID_TABLESPACE_SYSTEM_DISK_UNDO )
        {
            sSegMgmtType = SMI_SEGMENT_MGMT_CIRCULARLIST_TYPE;
        }
        else
        {
            sCacheHeader  =
                (sdpSpaceCacheCommon*) sddDiskMgr::getSpaceCache( aSpaceID );

            IDE_ASSERT( sCacheHeader != NULL );
            sSegMgmtType = sCacheHeader->mSegMgmtType;
        }
    }
    else
    {
        sSegMgmtType = SMI_SEGMENT_MGMT_NULL_TYPE;
    }

    return sSegMgmtType;
}

/***********************************************************************
 * Description : TableSpace�� extent �� page���� ��ȯ�Ѵ�.
 *
 *  aSpaceID - [IN] Ȯ���ϰ��� �ϴ� spage�� id
 **********************************************************************/
UInt sdpTableSpace::getPagesPerExt( scSpaceID     aSpaceID )
{
    sdpSpaceCacheCommon * sCacheHeader;
    UInt                  sPagesPerExt;

    if( sctTableSpaceMgr::hasState( aSpaceID, SCT_SS_INVALID_DISK_TBS ) == ID_FALSE )
    {
        sCacheHeader = (sdpSpaceCacheCommon*)sddDiskMgr::getSpaceCache( aSpaceID );
        IDE_ASSERT( sCacheHeader != NULL );

        sPagesPerExt = sCacheHeader->mPagesPerExt;
    }
    else
    {
        sPagesPerExt = 0;
    }

    return sPagesPerExt;

}



/*
 * ��ũ ���̺������̽��� Space Cache�� �Ҵ��ϰ� �ʱ�ȭ�Ѵ�.
 */
IDE_RC sdpTableSpace::doActAllocSpaceCache( idvSQL            * /*aStatistics*/,
                                            sctTableSpaceNode * aSpaceNode,
                                            void              * /*aActionArg*/ )
{
    sddTableSpaceNode   * sSpaceNode;
    sdpExtMgmtOp        * sTBSMgrOp;

    IDE_ASSERT( aSpaceNode != NULL );

    if ( sctTableSpaceMgr::isDiskTableSpace( aSpaceNode->mID )
         == ID_TRUE )
    {
        sSpaceNode = (sddTableSpaceNode*)aSpaceNode;
        sTBSMgrOp  = getTBSMgmtOP( sSpaceNode );

        IDE_ASSERT(sSpaceNode->mExtMgmtType == SMI_EXTENT_MGMT_BITMAP_TYPE );

        sSpaceNode = (sddTableSpaceNode*)aSpaceNode;

        IDE_ASSERT( sSpaceNode->mExtMgmtType == SMI_EXTENT_MGMT_BITMAP_TYPE );

        sTBSMgrOp  = getTBSMgmtOP( sSpaceNode );

        IDE_TEST( sTBSMgrOp->mInitialize( aSpaceNode->mID,
                                          sSpaceNode->mExtMgmtType,
                                          sSpaceNode->mSegMgmtType,
                                          sSpaceNode->mExtPageCount )
                  != IDE_SUCCESS );
    }

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*
 * ��ũ ���̺������̽��� Space Cache�� �����Ѵ�.
 */
IDE_RC sdpTableSpace::doActFreeSpaceCache( idvSQL            * /*aStatistics*/,
                                           sctTableSpaceNode * aSpaceNode,
                                           void              * /*aActionArg*/ )
{
    sdpExtMgmtOp * sTBSMgrOp;
    sdpSpaceCacheCommon * sCache;

    IDE_ASSERT( aSpaceNode != NULL );

    if ( sctTableSpaceMgr::isDiskTableSpace(aSpaceNode->mID)
         == ID_TRUE )
    {
        sTBSMgrOp  = getTBSMgmtOP(
            (sddTableSpaceNode*)aSpaceNode );

        sCache = (sdpSpaceCacheCommon *)sddDiskMgr::getSpaceCache( 
            aSpaceNode->mID );
        IDE_ASSERT( sCache != NULL );

        // Space Cache ����
        IDE_TEST( sTBSMgrOp->mDestroy( aSpaceNode->mID ) != IDE_SUCCESS );
    }

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*
 * ��ũ ���̺������̽��� Space Cache�� �����Ѵ�.
 *
 * BUG-29941 - SDP ��⿡ �޸� ������ �����մϴ�.
 * commit pending ���� ����� �� �Լ��� ȣ���Ͽ� tablespace�� �Ҵ��
 * Space Cache�� �����ϵ��� �Ѵ�.
 */
IDE_RC sdpTableSpace::freeSpaceCacheCommitPending(
                                           idvSQL            * /*aStatistics*/,
                                           sctTableSpaceNode * aSpaceNode,
                                           sctPendingOp      * /*aPendingOp*/ )
{
    IDE_TEST( doActFreeSpaceCache( NULL /* idvSQL */,
                                   aSpaceNode,
                                   NULL /* ActionArg */ ) != IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/* 
 * space cache �� ������ ���� ������ ��Ʈ�Ѵ�.
 * (��Ʈ���� ����� TBS���� ����ȴ�.)
 *  - GG�� extent �Ҵ簡�ɿ��κ�Ʈ
 *  - TBS�� ����ū GG id(��Ʈ�˻��û����)
 *  - extent�Ҵ������ GG ID��ȣ.(ó�� start�ÿ��� 0���� �ʱ�ȭ�Ѵ�.)
 */
IDE_RC sdpTableSpace::refineDRDBSpaceCache()
{

    IDE_TEST( sctTableSpaceMgr::doAction4EachTBS(
                                      NULL, /* aStatistics */
                                      doRefineSpaceCache,
                                      NULL, /* Action Argument*/
                                      SCT_ACT_MODE_NONE )
              != IDE_SUCCESS );

    return IDE_SUCCESS ;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/* TBS�� ���ؼ� Cache ������ Refine �Ѵ�. */
IDE_RC sdpTableSpace::doRefineSpaceCache( idvSQL            * /* aStatistics*/ ,
                                          sctTableSpaceNode * aSpaceNode,
                                          void              * /*aActionArg*/ )
{
    sddTableSpaceNode   * sSpaceNode;
    sdpExtMgmtOp        * sTBSMgrOp;

    IDE_ASSERT( aSpaceNode != NULL );

    if ( sctTableSpaceMgr::isDiskTableSpace( aSpaceNode->mID )
         == ID_TRUE )
    {
        sSpaceNode = (sddTableSpaceNode*)aSpaceNode;

        //temp tablespace�� ���� refine�� reset�ÿ� �ǽ��Ѵ�.
        if( sctTableSpaceMgr::isTempTableSpace( aSpaceNode->mID ) == ID_FALSE )
        {
            sTBSMgrOp  = getTBSMgmtOP( sSpaceNode );

            if( sTBSMgrOp->mRefineSpaceCache != NULL )
            {
                IDE_TEST( sTBSMgrOp->mRefineSpaceCache( sSpaceNode )
                        != IDE_SUCCESS );
            }

        }
   



    }

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*
 * Tablespace ��ȿȭ
 */
IDE_RC sdpTableSpace::alterTBSdiscard( sddTableSpaceNode  * aTBSNode )
{
    sdpExtMgmtOp * sTBSMgrOp;

    sTBSMgrOp  = getTBSMgmtOP( aTBSNode );

    IDE_TEST( sTBSMgrOp->mAlterDiscard( aTBSNode ) != IDE_SUCCESS );

    return IDE_SUCCESS ;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*
 * TableSpace Online/Offline ���� ����
 */
IDE_RC sdpTableSpace::alterTBSStatus( idvSQL*             aStatistics,
                                      void              * aTrans,
                                      sddTableSpaceNode * aSpaceNode,
                                      UInt                aState )
{
    sdpExtMgmtOp * sTBSMgrOp;

    sTBSMgrOp  = getTBSMgmtOP( (sddTableSpaceNode*)aSpaceNode );

    IDE_TEST( sTBSMgrOp->mAlterStatus( aStatistics,
                                       aTrans,
                                       aSpaceNode,
                                       aState )
              != IDE_SUCCESS );


    return IDE_SUCCESS ;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;

}


/*
 * ����Ÿ���� �ڵ�Ȯ�� ��� ����
 */
IDE_RC sdpTableSpace::alterDataFileAutoExtend( idvSQL      *aStatistics,
                                               void        *aTrans,
                                               scSpaceID    aSpaceID,
                                               SChar       *aFileName,
                                               idBool       aAutoExtend,
                                               ULong        aNextSize,
                                               ULong        aMaxSize,
                                               SChar       *aValidDataFileName)
{
    sdpExtMgmtOp * sTBSMgrOp;

    sTBSMgrOp  = getTBSMgmtOP( aSpaceID );

    IDU_FIT_POINT( "1.TASK-1842@sdpTableSpace::alterDataFileAutoExtend" );
    IDE_TEST( sTBSMgrOp->mAlterFileAutoExtend( aStatistics,
                                               aTrans,
                                               aSpaceID,
                                               aFileName,
                                               aAutoExtend,
                                               aNextSize,
                                               aMaxSize,
                                               aValidDataFileName )
              != IDE_SUCCESS );


    return IDE_SUCCESS ;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*
 * ����Ÿ���� ��� ����
 */
IDE_RC sdpTableSpace::alterDataFileName( idvSQL      *aStatistics,
                                         scSpaceID    aSpaceID,
                                         SChar       *aOldName,
                                         SChar       *aNewName )
{
    sdpExtMgmtOp * sTBSMgrOp;

    sTBSMgrOp  = getTBSMgmtOP( aSpaceID );

    IDE_TEST( sTBSMgrOp->mAlterFileName( aStatistics,
                                         aSpaceID,
                                         aOldName,
                                         aNewName ) != IDE_SUCCESS );
  
    return IDE_SUCCESS ;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*
 * �ϳ��� ����Ÿ ȭ���� ������ �ø���.
 */
IDE_RC sdpTableSpace::alterDataFileReSize( idvSQL       *aStatistics,
                                           void         *aTrans,
                                           scSpaceID     aSpaceID,
                                           SChar        *aFileName,
                                           ULong         aSizeWanted,
                                           ULong        *aSizeChanged,
                                           SChar        *aValidDataFileName )
{
    sdpExtMgmtOp * sTBSMgrOp;

    sTBSMgrOp  = getTBSMgmtOP( aSpaceID );

    IDU_FIT_POINT( "1.TASK-1842@dpTableSpace::alterDataFileReSize" );
    IDE_TEST( sTBSMgrOp->mAlterFileResize( aStatistics,
                                           aTrans,
                                           aSpaceID,
                                           aFileName,
                                           aSizeWanted,
                                           aSizeChanged,
                                           aValidDataFileName ) != IDE_SUCCESS );


    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*
 * 1�� �̻��� ����Ÿ���� �߰��Ѵ�.
 */
IDE_RC sdpTableSpace::createDataFiles( idvSQL             * aStatistics,
                                       void               * aTrans,
                                       scSpaceID            aSpaceID,
                                       smiDataFileAttr   ** aDataFileAttr,
                                       UInt                 aDataFileAttrCount )
{
    sdpExtMgmtOp * sTBSMgrOp;

    sTBSMgrOp  = getTBSMgmtOP( aSpaceID );

    IDU_FIT_POINT( "1.PROJ-1548@sdpTableSpace::createDataFiles" );
    IDE_TEST( sTBSMgrOp->mCreateFiles( aStatistics,
                                       aTrans,
                                       aSpaceID,
                                       aDataFileAttr,
                                       aDataFileAttrCount ) != IDE_SUCCESS );


    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*
 * �ϳ��� ����Ÿ ȭ���� �����Ѵ�.
 */
IDE_RC sdpTableSpace::removeDataFile( idvSQL         *aStatistics,
                                      void*           aTrans,
                                      scSpaceID       aSpaceID,
                                      SChar          *aFileName,
                                      SChar          *aValidDataFileName )
{
    sdpExtMgmtOp * sTBSMgrOp;

    sTBSMgrOp  = getTBSMgmtOP( aSpaceID );

    IDE_TEST( sTBSMgrOp->mDropFile( aStatistics,
                                    aTrans,
                                    aSpaceID,
                                    aFileName,
                                    aValidDataFileName ) != IDE_SUCCESS );


    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*
 * Tablespace�� �ڷᱸ���� ���Ἲ �����Ѵ�.
 */
IDE_RC sdpTableSpace::verify( idvSQL*   aStatistics,
                              scSpaceID aSpaceID,
                              UInt      aFlag )
{
    sdpExtMgmtOp * sTBSMgrOp;

    sTBSMgrOp = getTBSMgmtOP( aSpaceID );

    IDE_TEST( sTBSMgrOp->mVerify( aStatistics,
                                  aSpaceID,
                                  aFlag ) != IDE_SUCCESS );

    return IDE_SUCCESS ;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*
 * Tablespace�� �ڷᱸ���� ����Ѵ�.
 */
IDE_RC sdpTableSpace::dump( scSpaceID aSpaceID,
                            UInt      aDumpFlag )
{
    sdpExtMgmtOp * sTBSMgrOp;

    sTBSMgrOp  = getTBSMgmtOP( aSpaceID );

    IDE_TEST( sTBSMgrOp->mDump( aSpaceID, aDumpFlag ) != IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*
 * Tablespace Offline Commit Pending
 */
IDE_RC sdpTableSpace::alterOfflineCommitPending(
                                              idvSQL            * aStatistics,
                                              sctTableSpaceNode * aSpaceNode,
                                              sctPendingOp      * aPendingOp )
{
    sdpExtMgmtOp * sTBSMgrOp;

    sTBSMgrOp  = getTBSMgmtOP( aSpaceNode->mID );

    IDE_TEST( sTBSMgrOp->mAlterOfflineCommitPending( aStatistics,
                                                     aSpaceNode,
                                                     aPendingOp ) 
                  != IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/*
 * Tablespace Online Commit Pending
 */
IDE_RC sdpTableSpace::alterOnlineCommitPending(
                                              idvSQL            * aStatistics,
                                              sctTableSpaceNode * aSpaceNode,
                                              sctPendingOp      * aPendingOp )
{
    sdpExtMgmtOp * sTBSMgrOp;

    sTBSMgrOp  = getTBSMgmtOP( aSpaceNode->mID );

    IDE_TEST( sTBSMgrOp->mAlterOnlineCommitPending( aStatistics,
                                                    aSpaceNode,
                                                    aPendingOp ) 
                  != IDE_SUCCESS );

    return IDE_SUCCESS;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}
IDE_RC sdpTableSpace::allocExts( idvSQL          * aStatistics,
                                 sdrMtxStartInfo * aStartInfo,
                                 scSpaceID         aSpaceID,
                                 UInt              aOrgNrExts,
                                 sdpExtDesc      * aExtSlot )
{
    return sdptbExtent::allocExts( aStatistics,
                                   aStartInfo,
                                   aSpaceID,
                                   aOrgNrExts,
                                   aExtSlot );
}

IDE_RC sdpTableSpace::freeExt( idvSQL       * aStatistics,
                               sdrMtx       * aMtx,
                               scSpaceID      aSpaceID,
                               scPageID       aExtFstPID,
                               UInt         * aNrDone )
{
    sdpExtMgmtOp * sTBSMgrOp;

    sTBSMgrOp = getTBSMgmtOP( aSpaceID );

    return sTBSMgrOp->mFreeExtent( aStatistics,
                                   aMtx,
                                   aSpaceID,
                                   aExtFstPID,
                                   aNrDone );
}

/* ���̺������̽��� �� �������� ������ ���� ��ȯ */
IDE_RC sdpTableSpace::getTotalPageCount( idvSQL      * aStatistics,
                                         scSpaceID     aSpaceID,
                                         ULong       * aTotalPageCount )
{
    sdpExtMgmtOp        * sTBSMgrOp;

    sTBSMgrOp  = getTBSMgmtOP( aSpaceID );

    IDE_TEST( sTBSMgrOp->mGetTotalPageCount( aStatistics,
                                             aSpaceID,
                                             aTotalPageCount )
              != IDE_SUCCESS );

    return IDE_SUCCESS ;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/* BUG-15564 */
IDE_RC  sdpTableSpace::getAllocPageCount( idvSQL   *aStatistics,
                                          scSpaceID aSpaceID,
                                          ULong*    aAllocPageCount )
{
    sdpExtMgmtOp        * sTBSMgrOp;

    sTBSMgrOp  = getTBSMgmtOP( aSpaceID );

    IDE_TEST( sTBSMgrOp->mGetAllocPageCount( aStatistics,
                                             aSpaceID,
                                             aAllocPageCount )
              != IDE_SUCCESS );

    return IDE_SUCCESS ;

    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}

/***********************************************************************
 * Description: aSpaceID�� �ش��ϴ� TBS�� Free Extent Pool�� Item������ ��´�.
 *
 * aSpaceID - [IN] Tablespace ID
 ***********************************************************************/
ULong sdpTableSpace::getCachedFreeExtCount( scSpaceID aSpaceID )
{
    sdpExtMgmtOp * sTBSMgrOp;

    sTBSMgrOp  = getTBSMgmtOP( aSpaceID );

    return sTBSMgrOp->mGetCachedFreeExtCount( aSpaceID );
}



/**********************************************************************
 * Description: tbs�� ������ �����ϰų� �߰��ϱ����� �� ũ�Ⱑ valid���� �˻���.
 *
 *     [ �˻���� ] (BUG-29566 ������ ������ ũ�⸦ 32G �� �ʰ��Ͽ� �����ص�
 *                            ������ ������� �ʽ��ϴ�.)
 *        1. autoextend on �� ��� init size < max size����
 *        2. max size, Init size �� 32G Ȥ�� OS Limit�� ���� �ʴ���
 *           Ȥ�� �ʹ� ������ ������..
 *
 * aDataFileAttr        - [IN] �������ϴ� ���ϵ鿡 ���� ����
 * aDataFileAttrCount   - [IN] ���ϰ���
 * aValidSmallSize      - [IN] ������ �ּ� ũ��˻翡 ���� ��
 **********************************************************************/
IDE_RC sdpTableSpace::checkPureFileSize( smiDataFileAttr ** aDataFileAttr,
                                         UInt               aDataFileAttrCount,
                                         UInt               aValidSmallSize )
{
    UInt    i;
    UInt    sInitPageCnt;
    UInt    sMaxPageCnt;

    for( i=0; i < aDataFileAttrCount ; i++ )
    {
        sInitPageCnt = aDataFileAttr[i]->mInitSize;
        sMaxPageCnt  = aDataFileAttr[i]->mMaxSize;

        /*
         * BUG-26294 [SD] tbs������ maxsize�� initsize���� �������� 
         *                �ý��ۿ� ���� �����ϴ� ��찡 ����. 
         */
        // BUG-26632 [SD] Tablespace������ maxsize�� unlimited ��
        //           �����ϸ� �������� �ʽ��ϴ�.
        // Unlimited�̸� MaxSize�� 0���� �����˴ϴ�.
        // MaxSize�� 0�� ��� Init Size�� ������ �ʽ��ϴ�.
        // BUG-29566 ������ ������ ũ�⸦ 32G �� �ʰ��Ͽ� �����ص�
        //           ������ ������� �ʽ��ϴ�.
        // 1. Init Size < Max Size
        // 2. Max Size, Init Size�� ������ ������..
        if(( aDataFileAttr[i]->mIsAutoExtend == ID_TRUE ) &&
           ( sMaxPageCnt != 0 ))
        {
            IDE_TEST_RAISE( sInitPageCnt > sMaxPageCnt ,
                            error_initsize_exceed_maxsize );
        }

        if( sddDiskMgr::getMaxDataFileSize() < SD_MAX_FPID_COUNT )
        {
            // OS file limit size
            IDE_TEST_RAISE( sMaxPageCnt > sddDiskMgr::getMaxDataFileSize(),
                            error_maxsize_exceed_oslimit );
        }
        else
        {
            // 32G Maximum file size
            IDE_TEST_RAISE( sMaxPageCnt > SD_MAX_FPID_COUNT,
                            error_maxsize_exceed_maxfilesize );
        }

        if( sddDiskMgr::getMaxDataFileSize() < SD_MAX_FPID_COUNT )
        {
            IDE_TEST_RAISE( sInitPageCnt > sddDiskMgr::getMaxDataFileSize(),
                            error_initsize_exceed_oslimit );
        }
        else
        {
            IDE_TEST_RAISE( sInitPageCnt > SD_MAX_FPID_COUNT,
                            error_initsize_exceed_maxfilesize );
        }

        /*
         * BUG-20972
         * FELT������ �ϳ��� extentũ�⺸�� ����ũ�Ⱑ ������ ������ ó����
         */
        IDE_TEST_RAISE( sInitPageCnt < aValidSmallSize ,
                        error_data_file_is_too_small );
    }

    return IDE_SUCCESS ;

    IDE_EXCEPTION( error_data_file_is_too_small )
    {
        IDE_SET( ideSetErrorCode( smERR_ABORT_FILE_IS_TOO_SMALL,
                                  (ULong)sInitPageCnt,
                                  (ULong)aValidSmallSize ));
    }
    IDE_EXCEPTION( error_initsize_exceed_maxsize )
    {
        IDE_SET( ideSetErrorCode( smERR_ABORT_InitSizeExceedMaxSize ));
    }
    IDE_EXCEPTION( error_initsize_exceed_maxfilesize )
    {
        IDE_SET( ideSetErrorCode( smERR_ABORT_InitExceedMaxFileSize,
                                  (ULong)sInitPageCnt,
                                  (ULong)SD_MAX_FPID_COUNT) );
    }
    IDE_EXCEPTION( error_maxsize_exceed_maxfilesize )
    {
        IDE_SET( ideSetErrorCode( smERR_ABORT_MaxExceedMaxFileSize,
                                  (ULong)sMaxPageCnt,
                                  (ULong)SD_MAX_FPID_COUNT ) );
    }
    IDE_EXCEPTION( error_initsize_exceed_oslimit )
    {
        IDE_SET(ideSetErrorCode( smERR_ABORT_InitSizeExceedOSLimit ,
                                 (ULong)sInitPageCnt,
                                 (ULong)sddDiskMgr::getMaxDataFileSize() ));
    }
    IDE_EXCEPTION( error_maxsize_exceed_oslimit )
    {
        IDE_SET(ideSetErrorCode( smERR_ABORT_MaxSizeExceedOSLimit,
                                 (ULong)sMaxPageCnt,
                                 (ULong)sddDiskMgr::getMaxDataFileSize() ));
    }
    IDE_EXCEPTION_END;

    return IDE_FAILURE;
}