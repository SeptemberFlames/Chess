#include "pch.h"
#include "GameMainManage.h"
#include "Exception.h"
#include "RedisLoader.h"
#include "gameUserManage.h"
#include "NewMessageDefine.h"
#include "ErrorCode.h"
#include "log.h"
#include "PlatformMessage.h"
#include "Util.h"
#include "Function.h"
#include <string>
#include "BillManage.h"
#include "json/json.h"
#include "LoaderAsyncEvent.h"
#include <algorithm>

//////////////////////////////////////////////////////////////////////
CGameMainManage::CGameMainManage()
{
	m_uDeskCount = 0;
	m_pDesk = NULL;
	m_pDeskArray = NULL;
	m_uNameID = 0;
	m_pGameUserManage = NULL;
}

//////////////////////////////////////////////////////////////////////
CGameMainManage::~CGameMainManage()
{
	m_uDeskCount = 0;
	SafeDeleteArray(m_pDesk);
	SafeDeleteArray(m_pDeskArray);
	m_uNameID = 0;

	if (m_pGameUserManage)
	{
		m_pGameUserManage->Release();
	}
	SAFE_DELETE(m_pGameUserManage);
}

//////////////////////////////////////////////////////////////////////
bool CGameMainManage::OnInit(ManageInfoStruct* pInitData, KernelInfoStruct * pKernelData)
{
	if (!pInitData || !pKernelData)
	{
		return false;
	}

	//��ϷID
	m_uNameID = pKernelData->uNameID;

	// ��ҹ������
	m_pGameUserManage = new CGameUserManage;
	if (!m_pGameUserManage)
	{
		return false;
	}

	bool ret = m_pGameUserManage->Init();
	if (!ret)
	{
		return false;
	}

	// �ָ���Ϸ������
	int roomID = GetRoomID();
	ret = m_pRedis->CleanRoomAllData(roomID);
	if (!ret)
	{
		return false;
	}

	ret = InitGameDesk(pInitData->uDeskCount, pInitData->uDeskType);
	if (!ret)
	{
		return false;
	}

	return true;
}

//////////////////////////////////////////////////////////////////////
bool CGameMainManage::OnUnInit()
{
	m_uDeskCount = 0;

	SafeDeleteArray(m_pDesk);
	SafeDeleteArray(m_pDeskArray);

	return true;
}

//////////////////////////////////////////////////////////////////////
bool CGameMainManage::OnStart()
{
	// �������ݿ���
	if (m_pRedis)
	{
		m_pRedis->SetDBManage(&m_SQLDataManage);

		// ���ؽ�������
		LoadPoolData();
	}
	if (m_pRedisPHP)
	{
		m_pRedisPHP->SetDBManage(&m_SQLDataManage);
	}

	// ����һЩ��ʱ��
	SetTimer(LOADER_TIMER_SAVE_ROOM_PEOPLE_COUNT, CHECK_SAVE_ROOM_PEOPLE_COUNT * 1000);
	SetTimer(LOADER_TIMER_CHECK_REDIS_CONNECTION, CHECK_REDIS_CONNECTION_SECS * 1000);
	SetTimer(LOADER_TIMER_CHECK_INVALID_STATUS_USER, CHECK_INVALID_STATUS_USER_SECS * 1000);
	SetTimer(LOADER_TIMER_CHECK_TIMEOUT_DESK, CHECK_TIMEOUT_DESK_SECS * 1000);
	SetTimer(LOADER_TIMER_COMMON_TIMER, CHECK_COMMON_TIMER_SECS * 1000);

	if (m_InitData.iRoomSort == ROOM_SORT_HUNDRED)
	{
		SetTimer(LOADER_TIMER_HUNDRED_GAME_START, (3 + CUtil::GetRandNum() % 5) * 1000);
	}
	else if (m_InitData.iRoomSort == ROOM_SORT_SCENE)
	{
		SetTimer(LOADER_TIMER_SCENE_GAME_START, (3 + CUtil::GetRandNum() % 5) * 1000);
	}

	if (m_InitData.bCanCombineDesk && GetRoomType() == ROOM_TYPE_GOLD)
	{
		SetTimer(LOADER_TIMER_COMBINE_DESK_GAME_BENGIN, CHECK_COMBINE_DESK_GAME_BENGIN_SECS * 1000);
		m_combineUserSet.clear();
		m_combineRealUserVec.clear();
		m_combineRobotUserVec.clear();
	}

	return true;
}

//////////////////////////////////////////////////////////////////////
bool CGameMainManage::OnStop()
{
	KillAllTimer();

	return true;
}

//////////////////////////////////////////////////////////////////////
bool CGameMainManage::OnUpdate()
{
	// ���ؽ�������
	LoadPoolData();

	for (unsigned int i = 0; i < m_uDeskCount; i++)
	{
		CGameDesk* pDesk = GetDeskObject(i);
		if (pDesk == NULL)
		{
			continue;
		}

		if (pDesk->GetPlayGame())
		{
			pDesk->m_needLoadConfig = true;
		}
		else
		{
			pDesk->m_needLoadConfig = false;
			pDesk->LoadDynamicConfig();
		}
	}

	return true;
}

//////////////////////////////////////////////////////////////////////
bool CGameMainManage::OnSocketRead(NetMessageHead * pNetHead, void * pData, UINT uSize, ULONG uAccessIP, UINT uIndex, DWORD dwHandleID)
{
	if (!pNetHead)
	{
		return false;
	}

	// ͳ������
	WAUTOCOST("statistics message cost mainID: %d assistID: %d", pNetHead->uMainID, pNetHead->uAssistantID);

	int userID = pNetHead->uIdentification;
	if (userID <= 0)
	{
		ERROR_LOG("not send userID��OnSocketRead failed mainID=%d assistID=%d socketIdx:%d", pNetHead->uMainID, pNetHead->uAssistantID, uIndex);
		return false;
	}

	if (!m_pGServerConnect)
	{
		ERROR_LOG("m_pGServerConnect == NULL �޷���������");
		return false;
	}

	// �������⴦��
	if (pNetHead->uMainID == MSG_MAIN_USER_SOCKET_CLOSE)
	{
		return OnUserSocketClose(userID, uIndex);
	}

	// ��¼���⴦��
	if (pNetHead->uMainID == MSG_MAIN_LOADER_LOGON)
	{
		return OnHandleLogonMessage(pNetHead->uAssistantID, pData, uSize, uAccessIP, uIndex, userID);
	}

	// �ж��ڴ����Ƿ񻹴��ڸ���ң�������Ϣ���⴦���޸Ĳ���bug
	if (pNetHead->uMainID == MSG_MAIN_LOADER_ACTION && pNetHead->uAssistantID == MSG_ASS_LOADER_ACTION_STAND && GetUser(userID) == NULL)
	{
		m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_NOTIFY, MSG_NTF_LOADER_NO_USER, 0, userID);
		return false;
	}

	switch (pNetHead->uMainID)
	{
	case MSG_MAIN_LOADER_ACTION:			//�û�����
	{
		return OnHandleActionMessage(userID, pNetHead->uAssistantID, pData, uSize);
	}
	case MSG_MAIN_LOADER_FRAME:				//�����Ϣ
	{
		return OnHandleFrameMessage(userID, pNetHead->uAssistantID, pData, uSize);
	}
	case MSG_MAIN_LOADER_GAME:			// ��Ϸ��Ϣ
	{
		return OnHandleGameMessage(userID, pNetHead->uAssistantID, pData, uSize);
	}
	case MSG_MAIN_LOADER_DESKDISSMISS:
	{
		return OnHandleDismissMessage(userID, pNetHead->uAssistantID, pData, uSize);
	}
	case MSG_MAIN_LOADER_VOICEANDTALK:
	{
		return OnHandleVoiceAndTalkMessage(userID, pNetHead->uAssistantID, pData, uSize);
	}
	case MSG_MAIN_LOADER_MATCH:
	{
		return OnHandleMatchMessage(userID, pNetHead->uAssistantID, pData, uSize, dwHandleID);
	}
	default:
		break;
	}

	ERROR_LOG("unavalible mainID: %d", pNetHead->uMainID);

	return true;
}

//////////////////////////////////////////////////////////////////////////
bool CGameMainManage::OnSocketClose(ULONG uAccessIP, UINT uSocketIndex, UINT uConnectTime)
{
	// �������ø��ã�OnUserSocketClose
	return true;
}

//////////////////////////////////////////////////////////////////////
bool CGameMainManage::OnAsynThreadResult(AsynThreadResultLine * pResultData, void * pData, UINT uSize)
{
	if (!pResultData)
	{
		return false;
	}

	if (pResultData->uHandleKind == ANSY_THREAD_RESULT_TYPE_DATABASE)
	{

	}
	else if (pResultData->uHandleKind == ANSY_THREAD_RESULT_TYPE_HTTP)
	{
		char * pBuffer = (char *)pData;
		if (pBuffer == NULL)
		{
			ERROR_LOG("����phpʧ�ܣ�userID=%d,postType=%d", pResultData->uHandleID, pResultData->LineHead.uDataKind);
			return false;
		}

		if (strcmp(pBuffer, "0"))
		{
			ERROR_LOG("����phpʧ�ܣ�userID=%d,postType=%d,result=%s", pResultData->uHandleID, pResultData->LineHead.uDataKind, pBuffer);
			return false;
		}
	}
	else if (pResultData->uHandleKind == ANSY_THREAD_RESULT_TYPE_LOG)
	{

	}

	return false;
}

//////////////////////////////////////////////////////////////////////
CGameDesk* CGameMainManage::GetDeskObject(int deskIdx)
{
	CGameDesk *pDesk = NULL;
	if (deskIdx >= 0 && deskIdx < (int)m_uDeskCount)
	{
		pDesk = *(m_pDesk + deskIdx);
	}

	return pDesk;
}

//////////////////////////////////////////////////////////////////////
bool CGameMainManage::OnTimerMessage(UINT uTimerID)
{
	WAUTOCOST("��timerID: %d ����ʱ����ʱ ", uTimerID);

	//��Ϸ��ʱ��
	if (uTimerID >= TIME_START_ID)
	{
		int deskIdx = (int)((uTimerID - TIME_START_ID) / TIME_SPACE);
		if (deskIdx < (int)m_InitData.uDeskCount)
		{
			return (*(m_pDesk + deskIdx))->OnTimer((uTimerID - TIME_START_ID) % TIME_SPACE);
		}
	}

	//�ڲ���ʱ��
	switch (uTimerID)
	{
	case LOADER_TIMER_SAVE_ROOM_PEOPLE_COUNT:
	{
		OnSaveRoomPeopleCount();
		return true;
	}
	case LOADER_TIMER_CHECK_REDIS_CONNECTION:
	{
		CheckRedisConnection();
		return true;
	}
	case LOADER_TIMER_CHECK_INVALID_STATUS_USER:
	{
		OnTimerCheckInvalidStatusUser();
		return true;
	}
	case LOADER_TIMER_CHECK_TIMEOUT_DESK:
	{
		CheckTimeOutDesk();
		return true;
	}
	case LOADER_TIMER_COMMON_TIMER:
	{
		OnCommonTimer();
		return true;
	}
	case LOADER_TIMER_HUNDRED_GAME_START:
	{
		OnHundredGameStart();
		return true;
	}
	case LOADER_TIMER_SCENE_GAME_START:
	{
		OnSceneGameStart();
		return true;
	}
	case LOADER_TIMER_COMBINE_DESK_GAME_BENGIN:
	{
		OnCombineDeskGameBegin();
		return true;
	}
	default:
		break;
	}

	return true;
}

// ������ж�ʱ��
void CGameMainManage::KillAllTimer()
{
	for (unsigned int i = LOADER_TIMER_BEGIN + 1; i < LOADER_TIMER_END; i++)
	{
		KillTimer(i);
	}
}

//////////////////////////////////////////////////////////////////////
bool CGameMainManage::InitGameDesk(UINT uDeskCount, UINT uDeskType)
{
	//������Ϸ����
	UINT uDeskClassSize = 0;
	m_pDeskArray = CreateDeskObject(uDeskCount, uDeskClassSize);
	if (m_pDeskArray == NULL || uDeskClassSize == 0)
	{
		throw new CException(TEXT("CGameMainManage::InitGameDesk �ڴ�����ʧ��"), 0x418);
	}

	//�����ڴ�
	m_uDeskCount = uDeskCount;
	m_pDesk = new CGameDesk *[m_uDeskCount];
	if (m_pDesk == NULL)
	{
		throw new CException(TEXT("CGameMainManage::InitGameDesk �ڴ�����ʧ��"), 0x419);
	}

	//����ָ��
	for (UINT i = 0; i < m_uDeskCount; i++)
	{
		*(m_pDesk + i) = (CGameDesk *)((char *)m_pDeskArray + i*uDeskClassSize);
		(*(m_pDesk + i))->Init(i, m_KernelData.uDeskPeople, this);
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
//��ҵ���
bool CGameMainManage::OnUserSocketClose(int userID, UINT uSocketIndex)
{
	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		return false;
	}

	if (pUser->IsOnline == false)
	{
		ERROR_LOG("user is not online userID: %d", userID);
		return false;
	}

	pUser->IsOnline = false;
	pUser->socketIdx = -1;

#ifdef OFFLINE_CHANGE_AGREE_STATUS  //����ȡ��׼��
	// ׼��״̬��Ϊ����
	if (pUser->playStatus == USER_STATUS_AGREE)
	{
		pUser->playStatus = USER_STATUS_SITING;
	}
#endif

	CGameDesk* pDesk = GetDeskObject(pUser->deskIdx);

	// �Թ��߶���
	if (pUser->playStatus == USER_STATUS_WATCH && pDesk)
	{
		// ֱ������һص�����
		return pDesk->OnWatchUserOffline(pUser->userID);
	}

	// �������Թ��ߵ���
	if (GetRoomType() == ROOM_TYPE_MATCH && pUser->watchDeskIdx != INVALID_DESKIDX)
	{
		CGameDesk* pDeskWatch = GetDeskObject(pUser->watchDeskIdx);
		if (pDeskWatch)
		{
			pDeskWatch->MatchQuitMyDeskWatch(pUser, uSocketIndex, 0);
		}
	}

	if (pDesk)
	{
		int roomType = GetRoomType();
		if ((roomType == ROOM_TYPE_PRIVATE || roomType == ROOM_TYPE_FRIEND || roomType == ROOM_TYPE_FG_VIP)
			&& (m_pRedis && pDesk->m_iRunGameCount > 0 && pDesk->IsAllUserOffline()))
		{
			m_pRedis->SetPrivateDeskCheckTime(MAKE_DESKMIXID(m_InitData.uRoomID, pDesk->m_deskIdx), (int)time(NULL));
		}

		pDesk->UserNetCut(pUser);
	}
	else
	{
		if (m_InitData.bCanCombineDesk)
		{
			// ��ҵ��ߣ���ƥ���б���ɾ��
			DelCombineDeskUser(userID, pUser->isVirtual);
		}

		// �ߵ���ε��������
		// 1: �����������ɢ����һ�δ�ǳ�
		// 2: ��ҳ��뿪����֮��δ������һ������
		DelUser(userID);
	}

	return true;

}

//////////////////////////////////////////////////////////////////////
int CGameMainManage::GetUserPlayStatus(int userID)
{
	GameUserInfo* pUser = GetUser(userID);
	if (pUser)
	{
		return pUser->playStatus;
	}

	return 0;
}

//////////////////////////////////////////////////////////////////////
bool CGameMainManage::SetUserPlayStatus(int userID, int status)
{
	GameUserInfo* pUser = GetUser(userID);
	if (pUser)
	{
		pUser->playStatus = status;
		return true;
	}

	return false;
}

int CGameMainManage::GetOnlineUserCount()
{
	if (m_pGameUserManage)
	{
		return m_pGameUserManage->GetOnlineUserCount();
	}

	return 0;
}

bool CGameMainManage::IsCanWatch()
{
	GameBaseInfo* pGameBaseInfo = ConfigManage()->GetGameBaseInfo(m_uNameID);
	if (!pGameBaseInfo)
	{
		return false;
	}

	return pGameBaseInfo->canWatch == 1;
}

bool CGameMainManage::IsCanCombineDesk()
{
	GameBaseInfo* pGameBaseInfo = ConfigManage()->GetGameBaseInfo(m_uNameID);
	if (!pGameBaseInfo)
	{
		return false;
	}

	return pGameBaseInfo->canCombineDesk == 1;
}

bool CGameMainManage::IsMultiPeopleGame()
{
	GameBaseInfo* pGameBaseInfo = ConfigManage()->GetGameBaseInfo(m_uNameID);
	if (!pGameBaseInfo)
	{
		return false;
	}

	return pGameBaseInfo->multiPeopleGame == 1;
}

void CGameMainManage::NotifyUserRechargeMoney(int userID, long long rechargeMoney, int leftSecs)
{
	LoaderNotifyRecharge msg;
	msg.needRechargeMoney = rechargeMoney;
	msg.leftSecs = leftSecs;

	SendData(userID, &msg, sizeof(msg), MSG_MAIN_LOADER_NOTIFY, MSG_NTF_LOADER_RECHARGE, 0);
}

bool CGameMainManage::IsVisitorUser(int userID)
{
	UserData userData;
	if (m_pRedis->GetUserData(userID, userData) && userData.registerType == LOGON_VISITOR)
	{
		return true;
	}

	return false;
}

bool CGameMainManage::GoldRoomIsHaveDeskstation()
{
	if (m_InitData.iRoomSort == ROOM_SORT_HUNDRED)
	{
		for (int i = 0; i < (int)m_uDeskCount; i++)
		{
			CGameDesk *pGameDesk = GetDeskObject(i);
			if (pGameDesk && pGameDesk->IsCanSitDesk())
			{
				return true;
			}
		}

		return false;
	}

	return true;
}

bool CGameMainManage::LoadPoolData()
{
	if (!m_pRedis)
	{
		return false;
	}

	if (!m_pRedis->GetRewardsPoolInfo(m_InitData.uRoomID, m_rewardsPoolInfo))
	{
		ERROR_LOG("���ؽ�������ʧ��");
		return false;
	}

	return true;
}

int CGameMainManage::GetPoolConfigInfo(const char * fieldName)
{
	if (!fieldName)
	{
		return 0;
	}

	if (m_rewardsPoolInfo.detailInfo[0] != '\0')
	{
		Json::Reader reader;
		Json::Value value;
		if (!reader.parse(m_rewardsPoolInfo.detailInfo, value))
		{
			ERROR_LOG("parse json(%s) failed", m_rewardsPoolInfo.detailInfo);
			return 0;
		}

		if (value[fieldName].isString())
		{
			return atoi(value[fieldName].asString().c_str());
		}
		else if (value[fieldName].isInt())
		{
			return value[fieldName].asInt();
		}
		else
		{
			ERROR_LOG("�ֶδ���fieldName=%s", fieldName);
		}
	}

	return 0;
}

bool CGameMainManage::GetPoolConfigInfoString(const char * fieldName, int * pArray, int size, int &iArrayCount)
{
	if (!fieldName || !pArray)
	{
		return false;
	}

	iArrayCount = 0;

	if (m_rewardsPoolInfo.detailInfo[0] != '\0')
	{
		Json::Reader reader;
		Json::Value value;
		if (!reader.parse(m_rewardsPoolInfo.detailInfo, value))
		{
			ERROR_LOG("parse json(%s) failed", m_rewardsPoolInfo.detailInfo);
			return false;
		}

		Json::Value jsonValue = value[fieldName];

		if (jsonValue.size() > (UINT)size)
		{
			ERROR_LOG("json����");
			return false;
		}

		for (unsigned i = 0; i < jsonValue.size(); i++)
		{
			if (jsonValue[i].isInt())
			{
				pArray[i] = jsonValue[i].asInt();
				iArrayCount++;
			}
			else
			{
				ERROR_LOG("�ֶδ���fieldName=%s", fieldName);
				return false;
			}
		}

		return true;
	}

	return false;
}

bool CGameMainManage::IsOneToOnePlatform()
{
	OtherConfig otherConfig = ConfigManage()->GetOtherConfig();

	return otherConfig.byIsOneToOne == 0 ? false : true;
}

///////////////////////////////��½���///////////////////////////////////////
bool CGameMainManage::OnHandleLogonMessage(unsigned int assistID, void* pData, int size, unsigned int accessIP, unsigned int socketIdx, int userID)
{
	switch (assistID)
	{
		// ��¼
	case MSG_ASS_LOADER_LOGON:
	{
		return OnUserRequestLogon(pData, size, accessIP, socketIdx, userID);
	}
	// �ǳ�
	case MSG_ADD_LOADER_LOGOUT:
	{
		return OnUserRequestLogout(pData, size, accessIP, socketIdx, userID);
	}
	default:
		break;
	}

	return true;
}

///////////////////////////////������///////////////////////////////////////////
bool CGameMainManage::OnUserRequestLogon(void* pData, UINT uSize, ULONG uAccessIP, UINT uIndex, int userID)
{
	int roomType = GetRoomType();
	if (roomType == ROOM_TYPE_GOLD)
	{
		return OnMoneyLogonLogic(pData, uSize, uAccessIP, uIndex, userID);
	}
	else if (roomType == ROOM_TYPE_MATCH)
	{
		return OnMatchLogonLogic(pData, uSize, uAccessIP, uIndex, userID);
	}
	else if (roomType == ROOM_TYPE_FRIEND || roomType == ROOM_TYPE_PRIVATE || roomType == ROOM_TYPE_FG_VIP)
	{
		return OnPrivateLogonLogic(pData, uSize, uAccessIP, uIndex, userID);
	}

	return false;
}

bool CGameMainManage::OnPrivateLogonLogic(void* pData, UINT uSize, ULONG uAccessIP, UINT uIndex, int userID)
{
	if (uSize != sizeof(LoaderRequestLogon))
	{
		return false;
	}

	LoaderRequestLogon* pMessage = (LoaderRequestLogon *)pData;
	if (!pMessage)
	{
		return false;
	}

	UserData userData;
	if (!m_pRedis->GetUserData(userID, userData))
	{
		ERROR_LOG("��½ʧ�ܣ�GetUserData from redis failed: userID��%d", userID);
		return false;
	}

	if (userData.sealFinishTime != 0)
	{
		int iCurTime = (int)time(NULL);
		if (iCurTime < userData.sealFinishTime || userData.sealFinishTime < 0) //���ʱ�䵽�ˣ�����˺�
		{
			m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, ERROR_ACCOUNT_SEAL, userID);
			return false;
		}
	}

	if (!userData.isVirtual)
	{
		// ��ȡ������״̬
		int serverStatus = 0;
		m_pRedis->GetServerStatus(serverStatus);
		if (serverStatus == SERVER_PLATFROM_STATUS_CLOSE)
		{
			m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, ERROR_SERVER_CLOSE, userID);
			return false;
		}
	}

	int roomID = userData.roomID;
	if (roomID != GetRoomID() || pMessage->roomID != roomID)
	{
		ERROR_LOG("roomID is not match roomID(%d) userData.roomID(%d) pMessage->roomID(%d)", GetRoomID(), roomID, pMessage->roomID);
		return false;
	}

	int deskIdx = userData.deskIdx;
	if (userData.deskIdx == INVALID_DESKIDX)
	{
		ERROR_LOG("invalid deskStation userID: %d", userID);
		return false;
	}

	CGameDesk* pDesk = GetDeskObject(deskIdx);
	if (!pDesk)
	{
		ERROR_LOG("GetDeskObject failed deskIdx(%d)", deskIdx);
		return false;
	}

	int deskMixID = roomID * MAX_ROOM_HAVE_DESK_COUNT + deskIdx;
	PrivateDeskInfo privateDeskInfo;
	if (!m_pRedis->GetPrivateDeskRecordInfo(deskMixID, privateDeskInfo))
	{
		m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, ERROR_NO_THIS_DESK, userID);
		m_pRedis->SetUserDeskIdx(userID, INVALID_DESKIDX);
		m_pRedis->SetUserRoomID(userID, 0);
		ERROR_LOG("invalid deskMixID %d userID %d", deskMixID, userID);
		return false;
	}

	//�ж��Ƿ�����λ����������
	if (!pDesk->IsHaveDeskStation(userID, privateDeskInfo))
	{
		m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, ERROR_DESK_FULL, userID);
		m_pRedis->SetUserDeskIdx(userID, INVALID_DESKIDX);
		m_pRedis->SetUserRoomID(userID, 0);
		return false;
	}

	GameUserInfo* pUser = GetUser(userID);

	if (pUser && pUser->deskIdx != deskIdx)
	{
		ERROR_LOG("userID = %d �ڴ� deskIdx(%d) and redis deskIdx(%d) is not match��socketIdx = %d deskStation = %d,playStatus = %d",
			userID, pUser->deskIdx, deskIdx, pUser->socketIdx, pUser->deskStation, pUser->playStatus);

		// �ڴ�ͻ������Ƴ��������
		if (m_pGameUserManage)
		{
			m_pGameUserManage->DelUser(userID);
		}
		pUser = NULL;
	}

	if (!pUser)
	{
		pUser = new GameUserInfo;
		pUser->userID = userID;
		pUser->deskIdx = INVALID_DESKIDX;
		m_pGameUserManage->AddUser(pUser);
	}

	// ��ҵ�һЩ����
	pUser->IsOnline = true;
	pUser->socketIdx = uIndex;
	pUser->userStatus = userData.status;
	memcpy(pUser->name, userData.name, sizeof(pUser->name));
	memcpy(pUser->headURL, userData.headURL, sizeof(pUser->headURL));
	memcpy(pUser->longitude, userData.Lng, sizeof(pUser->longitude));
	memcpy(pUser->latitude, userData.Lat, sizeof(pUser->latitude));
	memcpy(pUser->address, userData.address, sizeof(pUser->address));
	pUser->isVirtual = userData.isVirtual;
	pUser->sex = userData.sex;
	pUser->money = userData.money;
	pUser->jewels = userData.jewels;
	memcpy(pUser->motto, userData.motto, sizeof(pUser->motto));
	strcpy(pUser->ip, userData.logonIP);

	LoaderResponseLogon msg;

	msg.isInDesk = true;
	msg.deskIdx = userData.deskIdx;
	msg.deskPasswdLen = strlen(privateDeskInfo.passwd);

	if (pUser->playStatus == USER_STATUS_DEFAULT)
	{
		if (IsCanWatch() == true)
		{
			msg.playStatus = USER_STATUS_WATCH;
		}
		else
		{
			msg.playStatus = USER_STATUS_SITING;
		}

	}
#ifdef OFFLINE_CHANGE_AGREE_STATUS  //����ȡ��׼��
	else if (pUser->playStatus == USER_STATUS_AGREE)
	{
		msg.playStatus = USER_STATUS_SITING;
	}
#endif
	else
	{
		msg.playStatus = pUser->playStatus;
	}

	memcpy(msg.deskPasswd, privateDeskInfo.passwd, strlen(privateDeskInfo.passwd));

	m_pGServerConnect->SendData(uIndex, &msg, sizeof(msg), MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, 0, userID);

	pDesk->OnPrivateUserLogin(userID, privateDeskInfo);

	return true;
}

bool CGameMainManage::OnMoneyLogonLogic(void* pData, UINT uSize, ULONG uAccessIP, UINT uIndex, int userID)
{
	if (uSize != sizeof(LoaderRequestLogon))
	{
		ERROR_LOG("size is not match");
		return false;
	}

	LoaderRequestLogon* pMessage = (LoaderRequestLogon *)pData;
	if (!pMessage)
	{
		ERROR_LOG("message is NULL");
		return false;
	}

	UserData userData;
	if (!m_pRedis->GetUserData(userID, userData))
	{
		ERROR_LOG("OnMoneyLogonLogic::GetUserData from redis failed: userID��%d", userID);
		return false;
	}

	if (userData.sealFinishTime != 0)
	{
		int iCurTime = (int)time(NULL);
		if (iCurTime < userData.sealFinishTime || userData.sealFinishTime < 0) //���ʱ�䵽�ˣ�����˺�
		{
			m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, ERROR_ACCOUNT_SEAL, userID);
			return false;
		}
	}

	if (!userData.isVirtual)
	{
		// ��ȡ������״̬
		int serverStatus = 0;
		m_pRedis->GetServerStatus(serverStatus);
		if (serverStatus == SERVER_PLATFROM_STATUS_CLOSE)
		{
			m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, ERROR_SERVER_CLOSE, userID);
			return false;
		}
	}

	RoomBaseInfo roomBasekInfo;
	RoomBaseInfo* pRoomBaseInfo = NULL;
	if (m_pRedis->GetRoomBaseInfo(GetRoomID(), roomBasekInfo))
	{
		pRoomBaseInfo = &roomBasekInfo;
	}
	else
	{
		pRoomBaseInfo = ConfigManage()->GetRoomBaseInfo(GetRoomID());
	}
	if (!pRoomBaseInfo)
	{
		ERROR_LOG("OnMoneyLogonLogic::GetRoomBaseInfo error");
		return false;
	}

	if (pRoomBaseInfo->minPoint > 0 && userData.money < pRoomBaseInfo->minPoint)
	{
		ERROR_LOG("user money is not enough usermoney=%lld minMoney=%d", userData.money, pRoomBaseInfo->minPoint);
		m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, ERROR_ROOM_NO_MONEYREQUIRE, userID);
		return false;
	}

	if (pRoomBaseInfo->minPoint > 0 && pRoomBaseInfo->maxPoint > pRoomBaseInfo->minPoint && userData.money > pRoomBaseInfo->maxPoint)
	{
		ERROR_LOG("user money is too much usermoney=%lld maxMoney=%d", userData.money, pRoomBaseInfo->maxPoint);
		m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, ERROR_ROOM_NO_MONEYREQUIRE, userID);
		return false;
	}

	if (0 != userData.roomID && GetRoomID() != userData.roomID)
	{
		if (userData.isVirtual)
		{
			m_pRedis->SetUserRoomID(userID, 0);
			m_pRedis->SetUserDeskIdx(userID, INVALID_DESKIDX);
		}
		else
		{
			LoaderResponseLogon msg;
			msg.iRoomID = userData.roomID;
			m_pGServerConnect->SendData(uIndex, &msg, sizeof(msg), MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, ERROR_ROOM_EXISTING, userID);
			return false;
		}
	}

	GameUserInfo* pUser = GetUser(userID);

	if (pUser && pUser->deskIdx == INVALID_DESKIDX)
	{
		//INFO_LOG("�û����������쳣 uIndex=%d,userID = %d,pUser->socketIdx = %d,pUser->deskStation = %d,pUser->playStatus = %d ",
		//	uIndex, userID, pUser->socketIdx, pUser->deskStation, pUser->playStatus);

		// �ڴ�ͻ������Ƴ��������
		if (m_pGameUserManage)
		{
			m_pGameUserManage->DelUser(userID);
		}
		pUser = NULL;
	}

	if (pUser)
	{
		if (INVALID_DESKIDX != pUser->deskIdx)
		{
			pUser->money = userData.money;
			pUser->jewels = userData.jewels;
			pUser->IsOnline = true;
			pUser->socketIdx = uIndex;
			pUser->userStatus = userData.status;
			pUser->isVirtual = userData.isVirtual;

			memcpy(pUser->longitude, userData.Lng, sizeof(pUser->longitude));
			memcpy(pUser->latitude, userData.Lat, sizeof(pUser->latitude));
			memcpy(pUser->address, userData.address, sizeof(pUser->address));
			memcpy(pUser->motto, userData.motto, sizeof(pUser->motto));

			LoaderResponseLogon msg;
			msg.isInDesk = true;
			m_pGServerConnect->SendData(uIndex, &msg, sizeof(msg), MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, 0, userID);

			CGameDesk * pGameDesk = GetDeskObject(pUser->deskIdx);
			if (pGameDesk)
			{
				pGameDesk->UserSitDesk(pUser);
			}
			else
			{
				ERROR_LOG("�û����Ӳ�����,userID = %d,pUser->deskIdx = %d", userID, pUser->deskIdx);
				return false;
			}
		}
		else
		{
			ERROR_LOG("�û����������쳣,userID = %d", userID);
			return false;
		}
	}
	else
	{
		// Ԥ���Ƿ���λ�ø��������
		if (!userData.isVirtual && !GoldRoomIsHaveDeskstation())
		{
			ERROR_LOG("û���㹻��λ������ userID=%d", userID);
			m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, ERROR_DESK_FULL, userID);
			return false;
		}

		// ����һ�������
		pUser = new GameUserInfo;
		if (!pUser)
		{
			ERROR_LOG("�����ڴ�ʧ��,userID = %d", userID);
			return false;
		}

		pUser->userID = userID;
		pUser->money = userData.money;
		pUser->jewels = userData.jewels;
		pUser->userStatus = userData.status;
		pUser->isVirtual = userData.isVirtual;
		memcpy(pUser->name, userData.name, sizeof(pUser->name));
		memcpy(pUser->headURL, userData.headURL, sizeof(pUser->headURL));
		pUser->sex = userData.sex;
		pUser->IsOnline = true;
		pUser->socketIdx = uIndex;
		memcpy(pUser->longitude, userData.Lng, sizeof(pUser->longitude));
		memcpy(pUser->latitude, userData.Lat, sizeof(pUser->latitude));
		memcpy(pUser->address, userData.address, sizeof(pUser->address));
		memcpy(pUser->motto, userData.motto, sizeof(pUser->motto));
		strcpy(pUser->ip, userData.logonIP);

		m_pGameUserManage->AddUser(pUser);

		LoaderResponseLogon msg;

		m_pGServerConnect->SendData(uIndex, &msg, sizeof(msg), MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, 0, userID);
	}

	// ��¼��½��¼
	if (!userData.isVirtual)
	{
		char tableName[128] = "";
		ConfigManage()->GetTableNameByDate(TBL_STATI_LOGON_LOGOUT, tableName, sizeof(tableName));

		BillManage()->WriteBill(&m_SQLDataManage, "INSERT INTO %s (userID,type,time,ip,platformType,macAddr) VALUES (%d,%d,%d,'%s',%d,'%s')",
			tableName, userID, 3, (int)time(NULL), userData.logonIP, GetRoomID(), "aa");
	}
	else
	{
		// �����˵�½�ȼ�¼redis�е�roomID
		m_pRedis->SetUserRoomID(userID, GetRoomID());
	}

	return true;
}

bool CGameMainManage::OnMatchLogonLogic(void* pData, UINT uSize, ULONG uAccessIP, UINT uIndex, int userID)
{
	if (uSize != sizeof(LoaderRequestLogon))
	{
		ERROR_LOG("size is not match");
		return false;
	}

	LoaderRequestLogon* pMessage = (LoaderRequestLogon *)pData;
	if (!pMessage)
	{
		ERROR_LOG("message is NULL");
		return false;
	}

	UserData userData;
	if (!m_pRedis->GetUserData(userID, userData))
	{
		ERROR_LOG("OnMoneyLogonLogic::GetUserData from redis failed: userID��%d", userID);
		return false;
	}

	if (userData.sealFinishTime != 0)
	{
		int iCurTime = (int)time(NULL);
		if (iCurTime < userData.sealFinishTime || userData.sealFinishTime < 0) //���ʱ�䵽�ˣ�����˺�
		{
			m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, ERROR_ACCOUNT_SEAL, userID);
			return false;
		}
	}

	if (!userData.isVirtual)
	{
		// ��ȡ������״̬
		int serverStatus = 0;
		m_pRedis->GetServerStatus(serverStatus);
		if (serverStatus == SERVER_PLATFROM_STATUS_CLOSE)
		{
			m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, ERROR_SERVER_CLOSE, userID);
			return false;
		}
	}

	if (0 != userData.roomID && GetRoomID() != userData.roomID)
	{
		LoaderResponseLogon msg;
		msg.iRoomID = userData.roomID;
		m_pGServerConnect->SendData(uIndex, &msg, sizeof(msg), MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, ERROR_ROOM_EXISTING, userID);
		return false;
	}

	//�������ж�
	if (userData.roomID == 0)
	{
		m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, ERROR_NO_USER_DATA, userID);
		return false;
	}

	GameUserInfo* pUser = GetUser(userID);

	if (pUser && pUser->deskIdx == INVALID_DESKIDX)
	{
		//INFO_LOG("�û����������쳣 uIndex=%d,userID = %d,pUser->socketIdx = %d,pUser->deskStation = %d,pUser->playStatus = %d ",
		//	uIndex, userID, pUser->socketIdx, pUser->deskStation, pUser->playStatus);

		// �ڴ�ͻ������Ƴ��������
		if (m_pGameUserManage)
		{
			m_pGameUserManage->DelUser(userID);
		}
		pUser = NULL;
	}

	// �����������Զ���¼������ϵͳ����ȥ
	if (pUser == NULL)
	{
		ERROR_LOG("û�б����ñ���,userID=%d", userID);
		m_pRedis->SetUserDeskIdx(userID, INVALID_DESKIDX);
		m_pRedis->SetUserRoomID(userID, 0);
		m_pRedis->SetUserMatchStatus(userID, MATCH_TYPE_NORMAL, USER_MATCH_STATUS_NORMAL);
		m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, ERROR_MATCH_OVER, userID);
		return false;
	}

	if (INVALID_DESKIDX != pUser->deskIdx)
	{
		pUser->money = userData.money;
		pUser->jewels = userData.jewels;
		pUser->IsOnline = true;
		pUser->socketIdx = uIndex;
		pUser->userStatus = userData.status;
		pUser->isVirtual = userData.isVirtual;

		memcpy(pUser->longitude, userData.Lng, sizeof(pUser->longitude));
		memcpy(pUser->latitude, userData.Lat, sizeof(pUser->latitude));
		memcpy(pUser->address, userData.address, sizeof(pUser->address));
		memcpy(pUser->motto, userData.motto, sizeof(pUser->motto));

		LoaderResponseLogon msg;
		msg.isInDesk = true;
		m_pGServerConnect->SendData(uIndex, &msg, sizeof(msg), MSG_MAIN_LOADER_LOGON, MSG_ASS_LOADER_LOGON, 0, userID);

		CGameDesk * pGameDesk = GetDeskObject(pUser->deskIdx);
		if (pGameDesk)
		{
			pGameDesk->UserSitDesk(pUser);
		}
		else
		{
			ERROR_LOG("�û����Ӳ�����,userID = %d,pUser->deskIdx = %d", userID, pUser->deskIdx);
			return false;
		}
	}
	else
	{
		ERROR_LOG("�û����������쳣,userID = %d", userID);
		return false;
	}

	// ��¼��½��¼
	if (!userData.isVirtual)
	{
		char tableName[128] = "";
		ConfigManage()->GetTableNameByDate(TBL_STATI_LOGON_LOGOUT, tableName, sizeof(tableName));

		BillManage()->WriteBill(&m_SQLDataManage, "INSERT INTO %s (userID,type,time,ip,platformType,macAddr) VALUES (%d,%d,%d,'%s',%d,'%s')",
			tableName, userID, 3, (int)time(NULL), userData.logonIP, GetRoomID(), "aa");
	}
	else
	{
		// �����˵�½�ȼ�¼redis�е�roomID
		m_pRedis->SetUserRoomID(userID, GetRoomID());
	}

	return true;
}

////////////////////////////////�ǳ����//////////////////////////////////////////
bool CGameMainManage::OnUserRequestLogout(void* pData, UINT size, ULONG uAccessIP, UINT uIndex, int userID)
{
	// ����ܷ��˳�
	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_LOGON, MSG_ADD_LOADER_LOGOUT, 0, userID);
		return false;
	}

	if (GetRoomType() == ROOM_TYPE_MATCH)
	{
		ERROR_LOG("���������ܵǳ�");
		return false;
	}

	int status = pUser->playStatus;
	if (status != USER_STATUS_STAND)
	{
		m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_LOGON, MSG_ADD_LOADER_LOGOUT, 0, userID);
		return false;
	}

	// ��ұ��벻�������ϲ��ܵǳ�
	if (pUser->deskIdx != INVALID_DESKIDX || pUser->deskStation != INVALID_DESKSTATION)
	{
		m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_LOGON, MSG_ADD_LOADER_LOGOUT, 0, userID);
		return false;
	}

	OnUserLogout(userID);

	// ���͵ǳ��ɹ�
	m_pGServerConnect->SendData(uIndex, NULL, 0, MSG_MAIN_LOADER_LOGON, MSG_ADD_LOADER_LOGOUT, 0, userID);

	return true;
}

//////////////////////////////////////////////////////////////////////
bool CGameMainManage::OnHandleActionMessage(int userID, unsigned int assistID, void * pData, int size)
{
	switch (assistID)
	{
	case MSG_ASS_LOADER_ACTION_SIT:
	{
		return OnHandleUserRequestSit(userID, pData, size);
	}
	case MSG_ASS_LOADER_ACTION_STAND:
	{
		return OnHandleUserRequestStand(userID, pData, size);
	}
	case MSG_ASS_LOADER_ACTION_MATCH_SIT:
	{
		return OnHandleUserRequestMatchSit(userID, pData, size);
	}
	case MSG_ASS_LOADER_ROBOT_TAKEMONEY:
	{
		return OnHandleRobotRequestTakeMoney(userID, pData, size);
	}
	case MSG_ASS_LOADER_GAME_BEGIN:
	{
		return OnHandleUserRequestGameBegin(userID);
	}
	case MSG_ASS_LOADER_ACTION_COMBINE_SIT:
	{
		return OnHandleUserRequestCombineSit(userID, pData, size);
	}
	case MSG_ASS_LOADER_ACTION_CANCEL_SIT:
	{
		return OnHandleUserRequestCancelSit(userID, pData, size);
	}
	default:
		break;
	}

	return false;
}

//////////////////////////////////////////////////////////////////////
bool CGameMainManage::OnHandleUserRequestSit(int userID, void* pData, int size)
{
	if (size != sizeof(LoaderRequestSit))
	{
		return false;
	}

	LoaderRequestSit* pMessage = (LoaderRequestSit *)pData;
	if (!pMessage)
	{
		return false;
	}

	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		ERROR_LOG("GetUser(%d) failed", userID);
		return false;
	}

	int deskIdx = pUser->deskIdx;
	CGameDesk * pDesk = GetDeskObject(deskIdx);
	if (!pDesk)
	{
		ERROR_LOG("GetDeskObject failed deskIdx:%d", deskIdx);
		return false;
	}

	// �趨ѡ�����λ��
	pUser->choiceDeskStation = pMessage->deskStation;

	return pDesk->UserSitDesk(pUser);
}

bool CGameMainManage::OnHandleUserRequestMatchSit(int userID, void* pData, int size)
{
	if (size != sizeof(LoaderRequestMatchSit))
	{
		ERROR_LOG("size != sizeof(LoaderRequestMatchSit)");
		return false;
	}

	LoaderRequestMatchSit* pMessage = (LoaderRequestMatchSit *)pData;
	if (!pMessage)
	{
		ERROR_LOG("pMessage=null");
		return false;
	}

	if (GetRoomType() == ROOM_TYPE_MATCH)
	{
		ERROR_LOG("����������ƥ������");
		return false;
	}

	if (GetRoomType() != ROOM_TYPE_GOLD)
	{
		ERROR_LOG("ֻ�н�ҳ�����ƥ��������userID=%d", userID);
		return false;
	}

	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		ERROR_LOG("GetUser(%d) failed", userID);
		return false;
	}

	// ����ģʽ���ܴ�������
	if (IsCanCombineDesk())
	{
		if (!pUser->isVirtual)
		{
			ERROR_LOG("�������ʧ�ܣ�������Ϸ���ܴ������£�userID=%d,name=%s", userID, pUser->name);
			SendErrNotifyMessage(userID, "������Ϸ���ܴ�������", SMT_EJECT);
		}
		return false;
	}

	RoomBaseInfo roomBasekInfo;
	RoomBaseInfo* pRoomBaseInfo = NULL;
	if (m_pRedis->GetRoomBaseInfo(m_InitData.uRoomID, roomBasekInfo))
	{
		pRoomBaseInfo = &roomBasekInfo;
	}
	else
	{
		pRoomBaseInfo = ConfigManage()->GetRoomBaseInfo(m_InitData.uRoomID);
	}
	if (!pRoomBaseInfo)
	{
		ERROR_LOG("m_InitData.uRoomID=%d������", m_InitData.uRoomID);
		return false;
	}

	//��ʼ����ѡ��������
	int iArrDeskIndex[G_CHANGEDESK_MAX_COUNT], iArrRealDeskIndex[G_CHANGEDESK_MAX_COUNT];
	int iArrDeskIndexCount = 0, iArrRealDeskIndexCount = 0;
	memset(iArrDeskIndex, INVALID_DESKIDX, sizeof(iArrDeskIndex));
	memset(iArrRealDeskIndex, INVALID_DESKIDX, sizeof(iArrRealDeskIndex));
	int iDeskIndex = INVALID_DESKIDX;

	CGameDesk * pGameDesk = NULL;
	if (INVALID_DESKIDX != pUser->deskIdx)
	{
		pGameDesk = GetDeskObject(pUser->deskIdx);
		if (pGameDesk && !pGameDesk->IsPlayGame(pUser->deskStation))
		{
			pGameDesk->UserLeftDesk(pUser);
		}
		else
		{
			ERROR_LOG("��Ϸ�в��ܻ���,userID=%d", userID);
			return false;
		}
	}

	if (m_InitData.iRoomSort == ROOM_SORT_HUNDRED)
	{
		for (int i = 0; i < (int)m_uDeskCount; i++)
		{
			pGameDesk = GetDeskObject(i);
			if (pGameDesk && pGameDesk->IsCanSitDesk())
			{
				if (pUser->isVirtual)
				{
					int iRandRobotCount = CUtil::GetRandRange(GetPoolConfigInfo("minC"), GetPoolConfigInfo("maxC"));
					if (iRandRobotCount <= 0)
					{
						iRandRobotCount = CUtil::GetRandRange(5, 20);
					}

					if ((int)pGameDesk->GetRobotPeople() > iRandRobotCount)
					{
						break;
					}
				}

				iDeskIndex = i;
				break;
			}
		}
	}
	else if (m_InitData.iRoomSort == ROOM_SORT_SCENE)
	{
		int iMaxUserNum = -1;
		int iSearchCount = 0;
		for (int i = 0; i < (int)m_uDeskCount; i++)
		{
			if (iArrDeskIndexCount >= G_CHANGEDESK_MAX_COUNT || iSearchCount >= (G_CHANGEDESK_MAX_COUNT + 1))
			{
				break;
			}
			pGameDesk = GetDeskObject(i);
			if (!pGameDesk)
			{
				ERROR_LOG("GetDeskObject(%d) failed:", i);
				continue;
			}

			if (i == pMessage->deskIdx)
			{
				// ԭ��������
				continue;
			}

			if (pGameDesk->IsCanSitDesk())
			{
				int iRealPeopleCount = pGameDesk->GetRealPeople();
				unsigned int uRobotPeopleCount = pGameDesk->GetRobotPeople();


				if (pUser->isVirtual && pRoomBaseInfo->robotCount > 0 && uRobotPeopleCount >= pRoomBaseInfo->robotCount)
				{
					iSearchCount++;
					continue;
				}

				if (iRealPeopleCount > 0)
				{
					iArrRealDeskIndex[iArrRealDeskIndexCount++] = i;
				}

				iArrDeskIndex[iArrDeskIndexCount++] = i;
			}
		}

		//���ѡ����
		if (iArrRealDeskIndexCount > 0)
		{
			iDeskIndex = iArrRealDeskIndex[CUtil::GetRandNum() % iArrRealDeskIndexCount];
		}
		else if (iArrDeskIndexCount > 0)
		{
			iDeskIndex = iArrDeskIndex[CUtil::GetRandNum() % iArrDeskIndexCount];
		}
	}
	else
	{
		int iMaxUserNum = -1;
		int iSearchCount = 0;
		for (int i = 0; i < (int)m_uDeskCount; i++)
		{
			if (iArrDeskIndexCount >= G_CHANGEDESK_MAX_COUNT || iSearchCount >= (G_CHANGEDESK_MAX_COUNT + 1))
			{
				break;
			}
			pGameDesk = GetDeskObject(i);
			if (!pGameDesk)
			{
				ERROR_LOG("GetDeskObject(%d) failed:", i);
				continue;
			}

			if (i == pMessage->deskIdx)
			{
				// ԭ��������
				continue;
			}

			if (pGameDesk->IsCanSitDesk())
			{
				int iRealPeopleCount = pGameDesk->GetRealPeople();
				unsigned int uRobotPeopleCount = pGameDesk->GetRobotPeople();

				if (pUser->isVirtual && iRealPeopleCount <= 0)
				{
					iSearchCount++;
					continue;
				}

				if (pUser->isVirtual && pRoomBaseInfo->robotCount > 0 && uRobotPeopleCount >= pRoomBaseInfo->robotCount)
				{
					iSearchCount++;
					continue;
				}

				if (iRealPeopleCount > 0)
				{
					iArrRealDeskIndex[iArrRealDeskIndexCount++] = i;
				}

				iArrDeskIndex[iArrDeskIndexCount++] = i;
			}
		}

		//���ѡ����
		if (iArrRealDeskIndexCount > 0)
		{
			iDeskIndex = iArrRealDeskIndex[CUtil::GetRandNum() % iArrRealDeskIndexCount];
		}
		else if (iArrDeskIndexCount > 0)
		{
			iDeskIndex = iArrDeskIndex[CUtil::GetRandNum() % iArrDeskIndexCount];
		}
	}

	if (INVALID_DESKIDX != iDeskIndex)
	{
		CGameDesk * pGameDesk = GetDeskObject(iDeskIndex);
		if (!pGameDesk)
		{
			ERROR_LOG("GetDeskObject failed deskIdx:%d", iDeskIndex);
			return false;
		}
		if (pGameDesk->UserSitDesk(pUser))
		{
			if (pGameDesk->GetRoomSort() == ROOM_SORT_NORMAL && pGameDesk->m_byBeginMode == FULL_BEGIN && pUser->deskStation != 255)
			{
				if (INVALID_DESKIDX != pMessage->deskIdx)
				{
					pGameDesk->UserAgreeGame(pUser->deskStation);
				}
			}
			return true;
		}
		else
		{
			ERROR_LOG("Fail");
			return false;
		}
	}
	else
	{
		m_pGServerConnect->SendData(pUser->socketIdx, NULL, 0, MSG_MAIN_LOADER_NOTIFY_USER, MSG_NTF_LOADER_DESK_USER_NOFIND_DESK, 0, pUser->userID);
		if (!pUser->isVirtual)
		{
			ERROR_LOG("û�и�������õ�����,userID=%d", pUser->userID);
		}
		return false;
	}

	return true;
}

bool CGameMainManage::OnHandleUserRequestStand(int userID, void * pData, int size)
{
	if (GetRoomType() == ROOM_TYPE_MATCH)
	{
		ERROR_LOG("����������վ��");
		return false;
	}

	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		ERROR_LOG("GetUser(%d) failed", userID);
		return true;
	}

	// ֻ����һ�������в���վ����
	if (pUser->deskIdx == INVALID_DESKIDX)
	{
		return false;
	}

	CGameDesk* pGameDesk = GetDeskObject(pUser->deskIdx);
	if (!pGameDesk)
	{
		ERROR_LOG("GetDeskObject failed deskIdx=%d", pUser->deskIdx);
		return false;
	}

	int roomSort = pGameDesk->GetRoomSort();
	int roomType = GetRoomType();
	if (roomSort == ROOM_SORT_NORMAL || roomSort == ROOM_SORT_SCENE)
	{
		if (roomType == ROOM_TYPE_PRIVATE || roomType == ROOM_TYPE_FRIEND || roomType == ROOM_TYPE_FG_VIP)
		{
			return pGameDesk->UserLeftDesk(pUser);
		}
		else
		{
			if (pGameDesk->IsPlayGame(pUser->deskStation))
			{
				return SendErrNotifyMessage(userID, "��Ϸ�в����뿪", SMT_EJECT);
			}
			else
			{
				return pGameDesk->UserLeftDesk(pUser);
			}
		}
	}
	else if (roomSort == ROOM_SORT_HUNDRED)
	{
		if (pGameDesk->IsPlayGame(pUser->deskStation))
		{
			return SendErrNotifyMessage(userID, "��Ϸ�в����뿪", SMT_EJECT);
		}
		else
		{
			return pGameDesk->UserLeftDesk(pUser);
		}
	}

	return true;
}

bool CGameMainManage::OnHandleRobotRequestTakeMoney(int userID, void* pData, int size)
{
	if (size != sizeof(_LoaderRobotTakeMoney))
	{
		ERROR_LOG("OnHandleRobotTakeMoney size match failed");
		return false;
	}

	_LoaderRobotTakeMoney* pMessage = (_LoaderRobotTakeMoney*)pData;
	if (!pMessage)
	{
		return false;
	}

	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		ERROR_LOG("OnHandleRobotRequestTakeMoney::GetUser(%d) failed", userID);
		return false;
	}

	if (!pUser->isVirtual || pMessage->iMoney <= 0)
	{
		return true;
	}

	//�����̨�����˽�ҷ�Χֱ�Ӱ��պ�̨��
	int iMinRobotHaveMoney_ = GetPoolConfigInfo("minM");
	int iMaxRobotHaveMoney_ = GetPoolConfigInfo("maxM");
	if (iMaxRobotHaveMoney_ > iMinRobotHaveMoney_ && m_InitData.iRoomSort != ROOM_SORT_HUNDRED)
	{
		pMessage->iMoney = CUtil::GetRandRange(iMinRobotHaveMoney_, iMaxRobotHaveMoney_);
	}

	//�����˴ӽ�����ȡ���
	__int64 _i64PoolMoney = 0;
	_i64PoolMoney = pUser->money - (__int64)pMessage->iMoney;
	//m_pRedis->SetRoomPoolMoney(GetRoomID(), _i64PoolMoney, true);
	m_pRedis->SetUserMoney(userID, pMessage->iMoney);
	pUser->money = pMessage->iMoney;

	_LoaderRobotTakeMoneyRes _res;

	_res.byCode = 0;
	_res.iMoney = (int)pUser->money;

	m_pGServerConnect->SendData(pUser->socketIdx, &_res, sizeof(_res), MSG_MAIN_LOADER_ACTION, MSG_ASS_LOADER_ROBOT_TAKEMONEY, 0, pUser->userID);

	return true;
}

bool CGameMainManage::OnHandleUserRequestGameBegin(int userID)
{
	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		ERROR_LOG("GetUser(%d) failed", userID);
		return false;
	}

	CGameDesk* pDesk = GetDeskObject(pUser->deskIdx);
	if (!pDesk)
	{
		return false;
	}

	return pDesk->OnUserRequsetGameBegin(userID);
}

bool CGameMainManage::OnHandleUserRequestCombineSit(int userID, void* pData, int size)
{
	if (size != 0)
	{
		return false;
	}

	if (m_InitData.iRoomSort == ROOM_SORT_HUNDRED || m_InitData.iRoomSort == ROOM_SORT_SCENE)
	{
		return false;
	}

	if (GetRoomType() == ROOM_TYPE_MATCH)
	{
		ERROR_LOG("��������������");
		return false;
	}

	if (GetRoomType() != ROOM_TYPE_GOLD)
	{
		return false;
	}

	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		ERROR_LOG("GetUser(%d) failed", userID);
		return false;
	}

	// û����������
	if (!IsCanCombineDesk())
	{
		if (!pUser->isVirtual)
		{
			ERROR_LOG("û����������������ʧ�ܣ�userID=%d,name=%s", userID, pUser->name);
		}
		return false;
	}

	CGameDesk * pGameDesk = NULL;
	if (INVALID_DESKIDX != pUser->deskIdx)
	{
		pGameDesk = GetDeskObject(pUser->deskIdx);
		if (pGameDesk && !pGameDesk->IsPlayGame(pUser->deskStation))
		{
			pGameDesk->UserLeftDesk(pUser);
		}
		else
		{
			ERROR_LOG("��Ϸ�в�������,userID=%d", userID);
			return false;
		}
	}

	// ��ӵ�����
	AddCombineDeskUser(userID, pUser->isVirtual);

	return true;
}

bool CGameMainManage::OnHandleUserRequestCancelSit(int userID, void* pData, int size)
{
	if (size != 0)
	{
		ERROR_LOG("size error");
		return false;
	}

	if (m_InitData.iRoomSort == ROOM_SORT_HUNDRED || m_InitData.iRoomSort == ROOM_SORT_SCENE)
	{
		return false;
	}

	if (GetRoomType() != ROOM_TYPE_GOLD)
	{
		ERROR_LOG("ֻ�н�ҳ�����ƥ��������userID=%d", userID);
		return false;
	}

	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		ERROR_LOG("GetUser(%d) failed", userID);
		return false;
	}

	// û����������
	if (!IsCanCombineDesk())
	{
		if (!pUser->isVirtual)
		{
			ERROR_LOG("û����������������ʧ�ܣ�userID=%d,name=%s", userID, pUser->name);
		}
		return false;
	}

	if (INVALID_DESKIDX != pUser->deskIdx)
	{
		ERROR_LOG("�Ѿ��������ڵ���Ҳ���������userID=%d", userID);
		return false;
	}

	if (!DelCombineDeskUser(userID, pUser->isVirtual))
	{
		ERROR_LOG("�Ŷ��б��в����ڸ����,userID=%d", userID);
		return false;
	}

	m_pGServerConnect->SendData(pUser->socketIdx, NULL, 0, MSG_MAIN_LOADER_ACTION, MSG_ASS_LOADER_ACTION_CANCEL_SIT, 0, pUser->userID);

	return true;
}

//////////////////////////////////////////////////////////////////////
bool CGameMainManage::OnHandleFrameMessage(int userID, unsigned int assistID, void* pData, int size)
{
	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		ERROR_LOG("GetUser(%d) failed", userID);
		return false;
	}

	if (pUser->deskIdx >= (int)m_uDeskCount)
	{
		return false;
	}

	// ѡ������
	CGameDesk* pGameDesk = GetDeskObject(pUser->deskIdx);
	if (!pGameDesk)
	{
		return false;
	}

	if (pUser->deskStation == INVALID_DESKSTATION)
	{
		ERROR_LOG("user(%d) deskStation is invalid", userID);
		return false;
	}

	bool isWatch = false;
	if (pUser->playStatus == USER_STATUS_WATCH)
	{
		isWatch = true;
	}

	return pGameDesk->HandleFrameMessage(pUser->deskStation, assistID, pData, size, isWatch);
}

//////////////////////////////////////////////////////////////////////
bool CGameMainManage::OnHandleGameMessage(int userID, unsigned int assistID, void* pData, int size)
{
	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		ERROR_LOG("GetUser(%d) failed", userID);
		return false;
	}

	CGameDesk * pGameDesk = GetDeskObject(pUser->deskIdx);
	if (!pGameDesk)
	{
		//ERROR_LOG("GetDeskObject:%d failed", pUser->deskIdx);
		return false;
	}

	if (pUser->deskStation == INVALID_DESKSTATION)
	{
		ERROR_LOG("user:%d deskStation is invalid", pUser->deskStation);
		return false;
	}

	// ��¼��Ҳ���ʱ��
	pUser->lastOperateTime = time(NULL);

	return pGameDesk->HandleNotifyMessage(pUser->deskStation, assistID, pData, size, pUser->playStatus == USER_STATUS_WATCH);
}

//////////////////////////////////////////////////////////////////////
bool CGameMainManage::OnHandleDismissMessage(int userID, unsigned int assistID, void * pData, int size)
{
	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		ERROR_LOG("GetUser(%d) failed", userID);
		return false;
	}

	CGameDesk * pGameDesk = GetDeskObject(pUser->deskIdx);
	if (!pGameDesk)
	{
		return false;
	}

	return pGameDesk->HandleDissmissMessage(pUser->deskStation, assistID, pData, size);
}

//////////////////////////////////////////////////////////////////////
bool CGameMainManage::OnHandleVoiceAndTalkMessage(int userID, unsigned int assistID, void * pData, int size)
{
	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		ERROR_LOG("GetUser(%d) failed", userID);
		return false;
	}

	CGameDesk * pGameDesk = GetDeskObject(pUser->deskIdx);
	if (!pGameDesk)
	{
		return false;
	}

	switch (assistID)
	{
	case MSG_ASS_LOADER_TALK:
	{
		pGameDesk->OnHandleTalkMessage(pUser->deskStation, pData, size);
	}
	case MSG_ASS_LOADER_VOICE:
	{
		pGameDesk->OnHandleVoiceMessage(pUser->deskStation, pData, size);
	}
	default:
		break;
	}

	return false;
}

//////////////////////////////////////////////////////////////////////////
bool CGameMainManage::OnHandleMatchMessage(int userID, unsigned int assistID, void* pData, int size, long handleID)
{
	switch (assistID)
	{
	case MSG_ASS_LOADER_MATCH_ALL_DESK_DATA:
	{
		return OnHandleMatchAllDeskStatusMessage(userID, pData, size, handleID);
	}
	case MSG_ASS_LOADER_MATCH_ENTER_WATCH_DESK:
	{
		return OnHandleMatchEnterWatchDeskMessage(userID, pData, size);
	}
	case MSG_ASS_LOADER_MATCH_QUIT_WATCH_DESK:
	{
		return OnHandleMatchQuitWatchDeskMessage(userID, pData, size);
	}
	default:
		break;
	}

	return false;
}

bool CGameMainManage::OnHandleMatchAllDeskStatusMessage(int userID, void* pData, int size, long handleID)
{
	SAFECHECK_MESSAGE(pMessage, LoaderRequestMatchAllDeskDataInfo, pData, size);

	auto itr = m_matchGameDeskMap.find(pMessage->llPartOfMatchID);
	if (itr == m_matchGameDeskMap.end())
	{
		ERROR_LOG("û�иñ���:%lld", pMessage->llPartOfMatchID);
		return false;
	}

	auto itrUser = m_matchUserMap.find(pMessage->llPartOfMatchID);
	if (itrUser == m_matchUserMap.end())
	{
		ERROR_LOG("û���ҵ��ñ���������%lld", pMessage->llPartOfMatchID);
		return false;
	}

	std::list<int> &deskList = itr->second;

	if (deskList.size() <= 0)
	{
		ERROR_LOG("�����Ѿ�������%lld", pMessage->llPartOfMatchID);
		return false;
	}

	LoaderResponseMatchAllDeskDataInfo msg;
	msg.deskCount = 0;
	msg.iCurPeopleCount = deskList.size() * m_KernelData.uDeskPeople;
	msg.iMaxPeopleCount = itrUser->second.size();

	for (auto itr = deskList.begin(); itr != deskList.end(); itr++)
	{
		CGameDesk* pDesk = GetDeskObject(*itr);
		if (!pDesk)
		{
			continue;
		}

		msg.iCurMatchRound = pDesk->m_iCurMatchRound;
		msg.iMaxMatchRound = pDesk->m_iMaxMatchRound;
		msg.desk[msg.deskCount].deskIdx = pDesk->m_deskIdx;

		if (pDesk->m_bFinishMatch)
		{
			msg.desk[msg.deskCount].status = 1;
		}
		else if (pDesk->m_llStartMatchTime == 0)
		{
			msg.desk[msg.deskCount].status = 0;
		}
		else
		{
			msg.desk[msg.deskCount].status = 2;
		}

		msg.deskCount++;
	}

	int iSendSize = 20 + msg.deskCount * sizeof(LoaderResponseMatchAllDeskDataInfo::DeskInfo);
	SendData(userID, &msg, iSendSize, MSG_MAIN_LOADER_MATCH, MSG_ASS_LOADER_MATCH_ALL_DESK_DATA, 0);

	return true;
}

//�Թ���������
bool CGameMainManage::OnHandleMatchEnterWatchDeskMessage(int userID, void* pData, int size)
{
	SAFECHECK_MESSAGE(pMessage, LoaderRequestMatchEnterWatch, pData, size);

	GameUserInfo *pUser = GetUser(userID);
	if (!pUser)
	{
		ERROR_LOG("�û������ڣ����ܽ����Թ�,userID=%d", userID);
		return false;
	}

	CGameDesk* pDesk = GetDeskObject(pMessage->deskIdx);
	if (!pDesk)
	{
		ERROR_LOG("�Թ۵����Ӳ�����:%d", pMessage->deskIdx);
		return false;
	}

	pDesk->MatchEnterMyDeskWatch(pUser, pMessage->llPartOfMatchID);

	return true;
}

//�˳��Թ�
bool CGameMainManage::OnHandleMatchQuitWatchDeskMessage(int userID, void* pData, int size)
{
	if (size != 0)
	{
		return false;
	}

	GameUserInfo *pUser = GetUser(userID);
	if (!pUser)
	{
		ERROR_LOG("�û������ڣ������˳��Թ�,userID=%d", userID);
		return false;
	}

	CGameDesk* pDesk = GetDeskObject(pUser->watchDeskIdx);
	if (!pDesk)
	{
		ERROR_LOG("���֮ǰû���Թ���������,watchDeskIdx=%d", pUser->watchDeskIdx);
		return false;
	}

	pDesk->MatchQuitMyDeskWatch(pUser, pUser->socketIdx, 1);

	return true;
}

//////////////////////////////////////////////////////////////////////
void CGameMainManage::SendData(int userID, void * pData, int size, unsigned int mainID, unsigned int assistID, unsigned int handleCode)
{
	if (!m_pGameUserManage)
	{
		ERROR_LOG("SendData m_pGameUserManage is NULL");
		return;
	}

	if (!m_pGServerConnect)
	{
		ERROR_LOG("SendData m_pGServerConnect is NULL");
		return;
	}

	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		ERROR_LOG("GetUser(%d) failed", userID);
		return;
	}

	if (!pUser->IsOnline)
	{
		return;
	}

	if (pUser->socketIdx == -1)
	{
		return;
	}

	m_pGServerConnect->SendData(pUser->socketIdx, pData, size, mainID, assistID, handleCode, userID);
}

bool CGameMainManage::SendErrNotifyMessage(int userID, LPCTSTR lpszMessage, int wType/* = SMT_EJECT*/)
{
	if (!lpszMessage || userID <= 0)
	{
		return false;
	}

	LoaderNotifyERRMsg msg;

	int sizeCount = strlen(lpszMessage);
	if (sizeCount == 0 || sizeCount >= sizeof(msg.notify))
	{
		return true;
	}

	msg.msgType = wType;
	msg.sizeCount = sizeCount;
	memcpy(msg.notify, lpszMessage, sizeCount);

	SendData(userID, &msg, 8 + msg.sizeCount, MSG_MAIN_LOADER_NOTIFY, MSG_NTF_LOADER_ERR_MSG, 0);

	return true;
}

//////////////////////////////////////////////////////////////////////
GameUserInfo * CGameMainManage::GetUser(int userID)
{
	if (!m_pGameUserManage)
	{
		return NULL;
	}

	return m_pGameUserManage->GetUser(userID);
}

bool CGameMainManage::OnUserLogout(int userID)
{
	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		ERROR_LOG("GetUser failed userID=%d", userID);
		return false;
	}

	// �����ڴ�ͻ�������
	DelUser(userID);

	return true;
}

//////////////////////////////////////////////////////////////////////
void CGameMainManage::DelUser(int userID)
{
	if (m_pGameUserManage)
	{
		m_pGameUserManage->DelUser(userID);
	}

	m_pRedis->SetUserRoomID(userID, 0);
	m_pRedis->SetUserDeskIdx(userID, INVALID_DESKIDX);
}

//////////////////////////////////////////////////////////////////////
int CGameMainManage::GetRoomID()
{
	return m_InitData.uRoomID;
}

//////////////////////////////////////////////////////////////////////
bool CGameMainManage::RemoveOfflineUser(int userID)
{
	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		ERROR_LOG("GetUser failed userID=%d", userID);
		return false;
	}

	if (pUser->IsOnline)
	{
		ERROR_LOG("user is online userID=%d", userID);
		return false;
	}

	// ���Ƴ���ʱ����ұ��봦��վ��״̬
	if (pUser->playStatus != USER_STATUS_STAND)
	{
		ERROR_LOG("user status is invalid userID=%d, status=%d", userID, pUser->playStatus);
		return false;
	}

	// �����ڴ�ͻ�������
	DelUser(userID);

	return true;
}

bool CGameMainManage::RemoveWatchUser(int userID)
{
	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		ERROR_LOG("GetUser failed userID=%d", userID);
		return false;
	}

	// �Ƿ��Թ�״̬
	if (pUser->playStatus != USER_STATUS_WATCH)
	{
		ERROR_LOG("user status is invalid userID=%d, status=%d", userID, pUser->playStatus);
		return false;
	}

	// ��������ڴ�
	DelUser(userID);

	return true;
}

void CGameMainManage::OnTimerCheckInvalidStatusUser()
{
	if (GetRoomType() == ROOM_TYPE_GOLD || GetRoomType() == ROOM_TYPE_MATCH)
	{
		return;
	}

	if (m_pGameUserManage)
	{
		m_pGameUserManage->CheckInvalidStatusUser(this);
	}
}

void CGameMainManage::CheckTimeOutDesk()
{
	if (GetRoomType() == ROOM_TYPE_GOLD || GetRoomType() == ROOM_TYPE_MATCH)
	{
		return;
	}

	if (!m_pRedis)
	{
		return;
	}

	int roomID = GetRoomID();

	time_t currTime = time(NULL);

	for (unsigned int i = 0; i < m_uDeskCount; i++)
	{
		CGameDesk* pDesk = GetDeskObject(i);
		if (!pDesk || !pDesk->IsEnable()) //����IsEnable��ֻ����ҽ��������ӣ�10���Ӳſ��Խ�ɢ������û�н��������Ӳ����ɢ
		{
			continue;
		}

		int deskMixID = MAKE_DESKMIXID(roomID, pDesk->m_deskIdx);

		PrivateDeskInfo privateDeskInfo;
		if (!m_pRedis->GetPrivateDeskRecordInfo(deskMixID, privateDeskInfo))
		{
			continue;
		}

		if (privateDeskInfo.masterID <= 0 || privateDeskInfo.roomID <= 0)
		{
			ERROR_LOG("��⵽��Ч������ deskMixID=%d", deskMixID);
			m_pRedis->DelPrivateDeskRecord(deskMixID);
			continue;
		}

		if (privateDeskInfo.friendsGroupID > 0 && privateDeskInfo.friendsGroupDeskNumber > 0)
		{
			continue;
		}

		//�ж��Ƿ�ʱ
		if (currTime - privateDeskInfo.checkTime <= PRIVATE_DESK_TIMEOUT_SECS)
		{
			continue;
		}

		// ��ǰû����һ��������˶�����
		if (privateDeskInfo.currDeskUserCount <= 0 || pDesk->IsAllUserOffline())
		{
			INFO_LOG("����ʱ���� roomID=%d,masterID=%d,passwd=%s",
				privateDeskInfo.roomID, privateDeskInfo.masterID, privateDeskInfo.passwd);
			pDesk->ProcessCostJewels();
			pDesk->ClearAllData(privateDeskInfo);
		}
	}
}

void CGameMainManage::OnCommonTimer()
{
	CheckTimeoutNotAgreeUser();

	CheckTimeoutNotOperateUser();

	if (GetRoomType() == ROOM_TYPE_MATCH)
	{
		CheckDeskStartMatch();
	}
}

void CGameMainManage::CheckRedisConnection()
{
	if (!m_pRedis)
	{
		ERROR_LOG("m_pRedis is NULL");
		return;
	}

	const RedisConfig& redisGameConfig = ConfigManage()->GetRedisConfig(REDIS_DATA_TYPE_GAME);

	m_pRedis->CheckConnection(redisGameConfig);

	if (m_pRedisPHP)
	{
		const RedisConfig& redisPHPConfig = ConfigManage()->GetRedisConfig(REDIS_DATA_TYPE_PHP);
		m_pRedisPHP->CheckConnection(redisPHPConfig);
	}
}

void CGameMainManage::CheckTimeoutNotAgreeUser()
{
	if (GetRoomType() != ROOM_TYPE_GOLD)
	{
		return;
	}

	if (m_InitData.iRoomSort == ROOM_SORT_HUNDRED || m_InitData.iRoomSort == ROOM_SORT_SCENE)
	{
		return;
	}

	time_t currTime = time(NULL);

	for (unsigned int i = 0; i < m_uDeskCount; i++)
	{
		CGameDesk* pDesk = GetDeskObject(i);
		if (pDesk && pDesk->IsEnable())
		{
			pDesk->CheckTimeoutNotAgreeUser(currTime);
		}
	}
}

// ��鳬ʱ���������
void CGameMainManage::CheckTimeoutNotOperateUser()
{
	if (GetRoomType() != ROOM_TYPE_GOLD)
	{
		return;
	}

	if (m_InitData.iRoomSort != ROOM_SORT_HUNDRED && m_InitData.iRoomSort != ROOM_SORT_SCENE)
	{
		return;
	}

	time_t currTime = time(NULL);

	for (unsigned int i = 0; i < m_uDeskCount; i++)
	{
		CGameDesk* pDesk = GetDeskObject(i);
		if (pDesk && pDesk->IsEnable())
		{
			pDesk->CheckTimeoutNotOperateUser(currTime);
		}
	}
}

void CGameMainManage::OnHundredGameStart()
{
	KillTimer(LOADER_TIMER_HUNDRED_GAME_START);

	RoomBaseInfo* pRoomBaseInfo = ConfigManage()->GetRoomBaseInfo(GetRoomID());
	if (pRoomBaseInfo->sort != ROOM_SORT_HUNDRED)
	{
		return;
	}

	for (unsigned int i = 0; i < m_uDeskCount; i++)
	{
		CGameDesk* pDesk = GetDeskObject(i);
		if (pDesk)
		{
			pDesk->OnStart();
		}
	}
}

void CGameMainManage::OnSceneGameStart()
{
	KillTimer(LOADER_TIMER_SCENE_GAME_START);

	RoomBaseInfo* pRoomBaseInfo = ConfigManage()->GetRoomBaseInfo(GetRoomID());
	if (pRoomBaseInfo->sort != ROOM_SORT_SCENE)
	{
		return;
	}

	for (unsigned int i = 0; i < m_uDeskCount; i++)
	{
		CGameDesk* pDesk = GetDeskObject(i);
		if (pDesk)
		{
			pDesk->OnStart();
		}
	}
}

// ���ڱ��浱ǰ��������
void CGameMainManage::OnSaveRoomPeopleCount()
{
	if (m_pRedis)
	{
		m_pRedis->SetRoomServerPeopleCount(GetRoomID(), m_pGameUserManage->GetUserCount());
	}
}

// ͳ��ƥ�������ٶ�
void CGameMainManage::OnCombineDeskGameBegin()
{
	// û�����˲���ʼ��Ϸ
	if (m_combineRealUserVec.size() <= 0)
	{
		return;
	}

	// �����ճ�һ��
	if (m_combineRealUserVec.size() + m_combineRobotUserVec.size() < m_KernelData.uDeskPeople)
	{
		return;
	}

	// ���
	m_allCombineDeskUserVec.clear();

	// �����˺ͻ����˷��뼯��
	for (size_t i = 0; i < m_combineRealUserVec.size(); i++)
	{
		m_allCombineDeskUserVec.push_back(m_combineRealUserVec[i]);
	}
	UINT uNeedRobotNums = m_combineRealUserVec.size() * (m_KernelData.uDeskPeople - 1);
	for (size_t i = 0; i < m_combineRobotUserVec.size() && i < uNeedRobotNums; i++)
	{
		m_allCombineDeskUserVec.push_back(m_combineRobotUserVec[i]);
	}

	// ���򼯺�
	random_shuffle(m_allCombineDeskUserVec.begin(), m_allCombineDeskUserVec.end());

	// �ҵ����ӣ������ȫ������
	for (int i = 0; i < (int)m_uDeskCount; i++)
	{
		if (m_allCombineDeskUserVec.size() < m_KernelData.uDeskPeople)
		{
			break;
		}

		CGameDesk *pGameDesk = GetDeskObject(i);
		if (!pGameDesk)
		{
			ERROR_LOG("GetDeskObject(%d) failed:", i);
			continue;
		}

		// �ҵ�һ�ſ�����������
		if (pGameDesk->IsCanSitDesk())
		{
			while (pGameDesk->GetUserCount() < pGameDesk->m_iConfigCount && !pGameDesk->IsPlayGame(0))
			{
				int tempUserID = m_allCombineDeskUserVec[m_allCombineDeskUserVec.size() - 1];
				GameUserInfo* pTempUser = GetUser(tempUserID);
				if (!pTempUser)
				{
					ERROR_LOG("GetUser(%d) failed", tempUserID);
					DelCombineDeskUser(tempUserID, pTempUser->isVirtual);
					m_allCombineDeskUserVec.pop_back();
					continue;
				}

				// ������ҷ���ƥ�������ɹ�
				m_pGServerConnect->SendData(pTempUser->socketIdx, NULL, 0, MSG_MAIN_LOADER_ACTION, MSG_ASS_LOADER_ACTION_COMBINE_SIT, 0, pTempUser->userID);

				if (!pGameDesk->UserSitDesk(pTempUser))
				{
					ERROR_LOG("UserSitDesk Fail");
				}

				// �Ŷ��б���ɾ��
				DelCombineDeskUser(tempUserID, pTempUser->isVirtual);
				m_allCombineDeskUserVec.pop_back();
			}

			if (!pGameDesk->IsPlayGame(0) && pGameDesk->GetUserCount() == pGameDesk->m_iConfigCount)
			{
				pGameDesk->GameBegin(0);
			}
		}
	}
}

int CGameMainManage::GetRoomType()
{
	return m_InitData.iRoomType;
}

//////////////////////////////////////////////////////////////////////////
// ����
bool CGameMainManage::AddCombineDeskUser(int userID, BYTE isVirtual)
{
	// �Ѿ�����
	if (m_combineUserSet.count(userID) > 0)
	{
		return false;
	}

	m_combineUserSet.insert(userID);

	if (isVirtual)
	{
		m_combineRobotUserVec.push_back(userID);
	}
	else
	{
		m_combineRealUserVec.push_back(userID);
	}

	return true;
}

bool CGameMainManage::DelCombineDeskUser(int userID, BYTE isVirtual)
{
	if (m_combineUserSet.count(userID) <= 0)
	{
		return false;
	}

	m_combineUserSet.erase(userID);

	if (isVirtual)
	{
		for (auto itr = m_combineRobotUserVec.begin(); itr != m_combineRobotUserVec.end(); itr++)
		{
			if (*itr == userID)
			{
				m_combineRobotUserVec.erase(itr);
				break;
			}
		}
	}
	else
	{
		for (auto itr = m_combineRealUserVec.begin(); itr != m_combineRealUserVec.end(); itr++)
		{
			if (*itr == userID)
			{
				m_combineRealUserVec.erase(itr);
				break;
			}
		}
	}

	return true;
}

//////////////////////////////////���ķ���Ϣ////////////////////////////////////////
bool CGameMainManage::SendMessageToCenterServer(UINT msgID, void * pData, UINT size, int userID/* = 0*/)
{
	if (m_pTcpConnect)
	{
		bool ret = m_pTcpConnect->Send(msgID, pData, size, userID);
		if (!ret)
		{
			ERROR_LOG("�����ķ���������ʧ�ܣ�����");
		}
		return ret;
	}
	else
	{
		ERROR_LOG("�����ķ���������ʧ�ܣ���m_pTcpConnect=NULL");
	}

	return false;
}

void CGameMainManage::SendResourcesChangeToLogonServer(int userID, int resourceType, long long value, int reason, long long changeValue)
{
	PlatformResourceChange msg;

	msg.resourceType = resourceType;
	msg.value = value;
	msg.reason = reason;
	msg.changeValue = changeValue;

	SendMessageToCenterServer(CENTER_MESSAGE_LOADER_RESOURCE_CHANGE, &msg, sizeof(msg), userID);
}

void CGameMainManage::SendFireCoinChangeToLogonServer(int friendsGroupID, int userID, long long value, int reason, long long changeValue)
{
	PlatformResourceChange msg;

	msg.reserveData = friendsGroupID;
	msg.value = value;
	msg.reason = reason;
	msg.changeValue = changeValue;

	SendMessageToCenterServer(CENTER_MESSAGE_LOADER_FIRECOIN_CHANGE, &msg, sizeof(msg), userID);
}

// ֪ͨȫ���󽱻��߹���
void CGameMainManage::SendRewardActivityNotify(const char * rewardMsg)
{
	if (rewardMsg == NULL)
	{
		ERROR_LOG("������Ϣ����Ϊ��");
		return;
	}

	LogonNotifyActivity msg;

	msg.sizeCount = strlen(rewardMsg) + 1;
	if (msg.sizeCount > sizeof(msg.content))
	{
		ERROR_LOG("������Ϣ̫��:%d", msg.sizeCount);
		return;
	}

	strcpy(msg.content, rewardMsg);

	SendMessageToCenterServer(CENTER_MESSAGE_LOADER_REWARD_ACTIVITY, &msg, sizeof(msg));
}

bool CGameMainManage::OnCenterServerMessage(UINT msgID, NetMessageHead * pNetHead, void* pData, UINT size, int userID)
{
	switch (msgID)
	{
	case CENTER_MESSAGE_COMMON_RESOURCE_CHANGE:
	{
		OnCenterMessageResourceChange(pData, size, userID);
		return true;
	}
	case PLATFORM_MESSAGE_FG_DISSMISS_DESK:
	{
		OnCenterMessageFGDissmissDesk(pData, size);
		return true;
	}
	case PLATFORM_MESSAGE_MASTER_DISSMISS_DESK:
	{
		OnCenterMessageMasterDissmissDesk(pData, size);
		return true;
	}
	case PLATFORM_MESSAGE_CLOSE_SERVER:
	{
		OnCenterCloseServerDissmissAllDesk(pData, size);
		return true;
	}
	case PLATFORM_MESSAGE_RELOAD_GAME_CONFIG:
	{
		OnCenterMessageReloadGameConfig(pData, size);
		return true;
	}
	case PLATFORM_MESSAGE_START_MATCH_PEOPLE:
	{
		OnCenterMessageStartMatchPeople(pData, size);
		return true;
	}
	case CENTER_MESSAGE_COMMON_START_MATCH_TIME:
	{
		OnCenterMessageStartMatchTime(pData, size);
		return true;
	}
	default:
		break;
	}
	return true;
}

bool CGameMainManage::OnCenterMessageResourceChange(void* pData, int size, int userID)
{
	if (size != sizeof(PlatformResourceChange))
	{
		return false;
	}

	PlatformResourceChange* pMessage = (PlatformResourceChange*)pData;
	if (!pMessage)
	{
		return false;
	}

	GameUserInfo* pUser = GetUser(userID);
	if (!pUser)
	{
		return true;
	}

	int roomType = GetRoomType();
	if (roomType == ROOM_TYPE_MATCH || roomType == ROOM_TYPE_PRIVATE)
	{
		return true;
	}

	if (pUser->deskIdx == INVALID_DESKIDX)
	{
		return true;
	}

	long long afterValue = 0;
	if (pMessage->resourceType == RESOURCE_TYPE_MONEY)
	{
		pUser->money += pMessage->changeValue;
		afterValue = pUser->money;
	}
	else if (pMessage->resourceType == RESOURCE_TYPE_FIRECOIN && roomType == ROOM_TYPE_FG_VIP)
	{
		pUser->fireCoin += (int)pMessage->changeValue;
		afterValue = pUser->fireCoin;
	}
	else if (pMessage->resourceType == RESOURCE_TYPE_JEWEL)
	{
		pUser->jewels += (int)pMessage->changeValue;
		afterValue = pUser->jewels;
	}
	else
	{
		return true;
	}

	if (roomType == ROOM_TYPE_MATCH || roomType == ROOM_TYPE_PRIVATE)
	{
		return true;
	}

	CGameDesk* pDesk = GetDeskObject(pUser->deskIdx);
	if (!pDesk)
	{
		ERROR_LOG("GetDeskObject failed deskIdx=%d", pUser->deskIdx);
		return false;
	}

	if (pMessage->resourceType == RESOURCE_TYPE_FIRECOIN && roomType == ROOM_TYPE_FG_VIP
		&& pDesk->m_friendsGroupMsg.friendsGroupID != pMessage->reserveData)
	{
		ERROR_LOG("���ֲ�id����ȷ,����friendsGroupID=%d,reserveData=%d", pDesk->m_friendsGroupMsg.friendsGroupID, pMessage->reserveData);
		return false;
	}

	pDesk->NotifyUserResourceChange(pUser->userID, pMessage->resourceType, afterValue, pMessage->changeValue);

	return true;
}

bool CGameMainManage::OnCenterMessageFGDissmissDesk(void* pData, int size)
{
	if (size != sizeof(PlatformDissmissDesk))
	{
		return false;
	}

	PlatformDissmissDesk* pMessage = (PlatformDissmissDesk*)pData;
	if (!pMessage)
	{
		return false;
	}

	int userID = pMessage->userID;
	int deskIdx = pMessage->deskMixID % MAX_ROOM_HAVE_DESK_COUNT;


	CGameDesk* pDesk = GetDeskObject(deskIdx);
	if (!pDesk)
	{
		ERROR_LOG("������ɢ����GetDeskObject failed deskIdx=%d", deskIdx);
		return false;
	}

	//��ɢ����
	pDesk->LogonDissmissDesk(userID, pMessage->bDelete);

	return true;
}

bool CGameMainManage::OnCenterMessageMasterDissmissDesk(void* pData, int size)
{
	if (size != sizeof(PlatformDissmissDesk))
	{
		return false;
	}

	PlatformDissmissDesk* pMessage = (PlatformDissmissDesk*)pData;
	if (!pMessage)
	{
		return false;
	}

	int deskIdx = pMessage->deskMixID % MAX_ROOM_HAVE_DESK_COUNT;

	CGameDesk* pDesk = GetDeskObject(deskIdx);
	if (!pDesk)
	{
		ERROR_LOG("������ɢ����GetDeskObject failed deskIdx=%d", deskIdx);
		return false;
	}

	//��ɢ����
	pDesk->LogonDissmissDesk();

	return true;
}

// �ط�����ɢ��������
void CGameMainManage::OnCenterCloseServerDissmissAllDesk(void* pData, int size)
{
	if (size != 0)
	{
		ERROR_LOG("�ط�����ʧ�ܡ����ݰ���С����");
		return;
	}

	if (GetRoomType() == ROOM_TYPE_GOLD || GetRoomType() == ROOM_TYPE_MATCH)
	{
		return;
	}

	if (!m_pRedis)
	{
		return;
	}

	int roomID = GetRoomID();

	std::vector<int> vecRooms;
	m_pRedis->GetAllDesk(roomID, vecRooms);
	int iSaveCount = 0;
	for (size_t i = 0; i < vecRooms.size(); i++)
	{
		int deskIdx = vecRooms[i];
		int deskMixID = MAKE_DESKMIXID(roomID, deskIdx);

		PrivateDeskInfo privateDeskInfo;
		if (!m_pRedis->GetPrivateDeskRecordInfo(deskMixID, privateDeskInfo))
		{
			continue;
		}

		CGameDesk* pDesk = GetDeskObject(deskIdx);
		if (!pDesk)
		{
			continue;
		}

		if (privateDeskInfo.friendsGroupID > 0 && privateDeskInfo.friendsGroupDeskNumber > 0)
		{
			if (m_pRedis->SaveFGDeskRoom(privateDeskInfo))
			{
				iSaveCount++;
			}
		}

		//��ɢ�������ӣ�ǿ�н�ɢ��
		pDesk->LogonDissmissDesk();
	}

	INFO_LOG("======== LoaderServer �رճɹ� ,�ر���������%d��������������%d=======", vecRooms.size(), iSaveCount);
}

// ���¼�����������
bool CGameMainManage::OnCenterMessageReloadGameConfig(void* pData, int size)
{
	if (size != 0)
	{
		ERROR_LOG("��ƥ��");
		return false;
	}

	OnUpdate();

	return true;
}

// ��ʼ����(ʵʱ��)
bool CGameMainManage::OnCenterMessageStartMatchPeople(void* pData, int size)
{
	// ͳ������
	WAUTOCOST("PHP����ʼ������ʱ��");

	SAFECHECK_MESSAGE(pMessage, PlatformPHPReqStartMatchPeople, pData, size);

	if (!m_pRedis)
	{
		return false;
	}

	if (GetRoomType() != ROOM_TYPE_MATCH)
	{
		ERROR_LOG("�������Ͳ���ȷ,roomType=%d", GetRoomType());
		return false;
	}

	if (pMessage->gameID != m_uNameID)
	{
		ERROR_LOG("��Ϸid��ƥ�䣬gameID=%d,pMessage->gameID=%d", m_uNameID, pMessage->gameID);
		return false;
	}

	GameBaseInfo * pGameBaseInfo = ConfigManage()->GetGameBaseInfo(m_uNameID);
	if (!pGameBaseInfo)
	{
		ERROR_LOG("û�����ø���Ϸ��gameID=%d", m_uNameID);
		return false;
	}

	//����һ���ֲ�����id
	long long llPartOfMatchID = m_pRedis->GetPartOfMatchIndex();
	if (llPartOfMatchID <= 0)
	{
		ERROR_LOG("��������ʧ��,llPartOfMatchID=%lld", llPartOfMatchID);
		return false;
	}

	//��ȡ��������
	std::vector<MatchUserInfo> vecPeople;
	if (!m_pRedis->GetFullPeopleMatchPeople(m_uNameID, pMessage->matchID, pMessage->peopleCount, vecPeople))
	{
		ERROR_LOG("���������������޷���������ǰ����=%d����Ҫ����=%d", vecPeople.size(), pMessage->peopleCount);
		return true;
	}

	//��ȡ����������
	std::list<int> matchDeskList;
	if (!GetMatchDesk(pMessage->peopleCount / pGameBaseInfo->deskPeople, matchDeskList))
	{
		return true;
	}

	//����ر�����Ա��ȫ����������
	for (size_t i = 0; i < vecPeople.size(); i++)
	{
		int userID = vecPeople[i].userID;
		UserData userData;
		if (!m_pRedis->GetUserData(userID, userData))
		{
			ERROR_LOG("��ȡ�������ʧ�ܣ������޷�����,uerID=%d", userID);
			return false;
		}

		// ����״̬
		vecPeople[i].byMatchStatus = DESK_MATCH_STATUS_NORMAL;

		// �ڴ�����
		GameUserInfo* pUser = m_pGameUserManage->GetUser(userID);
		if (pUser)
		{
			m_pGameUserManage->DelUser(userID);
			pUser = NULL;
		}

		// ����һ�������
		pUser = new GameUserInfo;
		if (!pUser)
		{
			ERROR_LOG("�޷������������ڴ�ʧ��,userID = %d", userID);
			return false;
		}

		//��ʼ��������Ϣ
		pUser->matchSocre = 0;

		//����ڴ�����
		pUser->userID = userID;
		pUser->money = userData.money;
		pUser->jewels = userData.jewels;
		pUser->userStatus = userData.status;
		pUser->isVirtual = userData.isVirtual;
		memcpy(pUser->name, userData.name, sizeof(pUser->name));
		memcpy(pUser->headURL, userData.headURL, sizeof(pUser->headURL));
		pUser->sex = userData.sex;
		pUser->IsOnline = false;
		pUser->socketIdx = -1;
		if (pUser->isVirtual == 1)
		{
			RobotPositionInfo positionInfo;
			int iRobotIndex = m_pRedis->GetRobotInfoIndex();
			ConfigManage()->GetRobotPositionInfo(iRobotIndex, positionInfo);
			strcpy(pUser->longitude, positionInfo.longitude.c_str());
			strcpy(pUser->latitude, positionInfo.latitude.c_str());
			strcpy(pUser->address, positionInfo.address.c_str());
			strcpy(pUser->ip, positionInfo.ip.c_str());
			strcpy(pUser->name, positionInfo.name.c_str());
			strcpy(pUser->headURL, positionInfo.headUrl.c_str());
			pUser->sex = positionInfo.sex;
		}
		else
		{
			memcpy(pUser->longitude, userData.Lng, sizeof(pUser->longitude));
			memcpy(pUser->latitude, userData.Lat, sizeof(pUser->latitude));
			memcpy(pUser->address, userData.address, sizeof(pUser->address));
			memcpy(pUser->motto, userData.motto, sizeof(pUser->motto));
		}

		m_pGameUserManage->AddUser(pUser);
	}

	time_t currTime = time(NULL);

	//��ʼ����������
	for (auto itr = matchDeskList.begin(); itr != matchDeskList.end(); itr++)
	{
		CGameDesk * pGameDesk = GetDeskObject(*itr);
		if (pGameDesk == NULL)
		{
			ERROR_LOG("### �����ڴ��Ҳ���,index=%d ###", *itr);
			return false;
		}

		pGameDesk->InitDeskMatchData();
		pGameDesk->m_llPartOfMatchID = llPartOfMatchID;
		pGameDesk->m_iCurMatchRound = 1;
		pGameDesk->m_iMaxMatchRound = pMessage->matchRound;
		pGameDesk->m_llStartMatchTime = currTime + 10; //���ÿ�ʼ����ʱ��
	}

	//֪ͨ��Щ��ҹ����μӱ���
	PlatformLoaderNotifyStartMatch notifyMsg;
	notifyMsg.gameID = m_uNameID;
	notifyMsg.matchType = MATCH_TYPE_PEOPLE;
	notifyMsg.matchID = pMessage->matchID;
	notifyMsg.peopleCount = pMessage->peopleCount;
	notifyMsg.roomID = GetRoomID();
	for (size_t i = 0; i < vecPeople.size(); i++)
	{
		notifyMsg.userID[i] = vecPeople[i].userID;
	}
	SendMessageToCenterServer(CENTER_MESSAGE_LOADER_NOTIFY_START_MATCH, &notifyMsg, sizeof(notifyMsg));

	//�����Ӽ����ڴ�
	m_matchGameDeskMap.insert(std::make_pair(llPartOfMatchID, matchDeskList));

	//��������Ա���ڴ��б���
	m_matchUserMap.insert(std::make_pair(llPartOfMatchID, vecPeople));

	//���������ID
	m_matchMainIDMap[llPartOfMatchID] = pMessage->matchID;

	//�����������
	m_matchTypeMap[llPartOfMatchID] = MATCH_TYPE_PEOPLE;

	//����Щ������Ա��redis��ɾ��
	m_pRedis->DelFullPeopleMatchPeople(m_uNameID, pMessage->matchID, vecPeople);

	//��ʼ����
	AllocDeskStartMatch(matchDeskList, vecPeople);

	return true;
}

// ��ʼ����(��ʱ��)
bool CGameMainManage::OnCenterMessageStartMatchTime(void* pData, int size)
{
	// ͳ������
	WAUTOCOST("��ʱ����ʼ������ʱ��");

	SAFECHECK_MESSAGE(pMessage, PlatformReqStartMatchTime, pData, size);

	time_t currTime = time(NULL);

	if (!m_pRedis)
	{
		return false;
	}

	if (GetRoomType() != ROOM_TYPE_MATCH)
	{
		ERROR_LOG("�������Ͳ���ȷ,roomType=%d", GetRoomType());
		return false;
	}

	if (pMessage->gameID != m_uNameID)
	{
		ERROR_LOG("��Ϸid��ƥ�䣬gameID=%d,pMessage->gameID=%d", m_uNameID, pMessage->gameID);
		return false;
	}

	GameBaseInfo * pGameBaseInfo = ConfigManage()->GetGameBaseInfo(m_uNameID);
	if (!pGameBaseInfo)
	{
		ERROR_LOG("û�����ø���Ϸ��gameID=%d", m_uNameID);
		SendMatchFailMail(MATCH_FAIL_REASON_SYSTEM_ERROR, 0, MATCH_TYPE_TIME, pMessage->matchID);
		return false;
	}

	//����һ���ֲ�����id
	long long llPartOfMatchID = m_pRedis->GetPartOfMatchIndex();
	if (llPartOfMatchID <= 0)
	{
		ERROR_LOG("��������ʧ��,llPartOfMatchID=%lld", llPartOfMatchID);
		SendMatchFailMail(MATCH_FAIL_REASON_SYSTEM_ERROR, 0, MATCH_TYPE_TIME, pMessage->matchID);
		return false;
	}

	//��ȡ��������
	std::vector<MatchUserInfo> vecPeople;
	if (!m_pRedis->GetTimeMatchPeople(pMessage->matchID, vecPeople))
	{
		ERROR_LOG("�޷�������matchID=%d ��ǰ����=%d", pMessage->matchID, vecPeople.size());
		SendMatchFailMail(MATCH_FAIL_REASON_NOT_ENOUGH_PEOPLE, 0, MATCH_TYPE_TIME, pMessage->matchID);
		return true;
	}

	//�����������
	if (vecPeople.size() > MAX_MATCH_PEOPLE_COUNT)
	{
		ERROR_LOG("���������������ƣ���ǰ����=%d", vecPeople.size());
		SendMatchFailMail(MATCH_FAIL_REASON_SYSTEM_ERROR, 0, MATCH_TYPE_TIME, pMessage->matchID);
		return true;
	}

	//��Ч��Ҽ����ڴ档ʧ�������ڴ�
	for (auto itr = vecPeople.begin(); itr != vecPeople.end();)
	{
		int userID = itr->userID;
		UserData userData;
		if (!m_pRedis->GetUserData(userID, userData))
		{
			ERROR_LOG("��ȡ�������ʧ��,uerID=%d", userID);
			itr = vecPeople.erase(itr);
			continue;
		}

		//��Ϸ�У���Ϊ��Ȩ
		if (userData.roomID > 0)
		{
			SendMatchFailMail(MATCH_FAIL_REASON_PLAYING, userID, MATCH_TYPE_TIME, pMessage->matchID);
			ClearMatchStatus(MATCH_FAIL_REASON_PLAYING, userID, MATCH_TYPE_TIME, pMessage->matchID);
			itr = vecPeople.erase(itr);
			continue;
		}

		// ����״̬
		itr->byMatchStatus = DESK_MATCH_STATUS_NORMAL;

		// �ڴ�����
		GameUserInfo* pUser = m_pGameUserManage->GetUser(userID);
		if (pUser)
		{
			m_pGameUserManage->DelUser(userID);
			pUser = NULL;
		}

		// ����һ�������
		pUser = new GameUserInfo;
		if (!pUser)
		{
			ERROR_LOG("�����ڴ�ʧ��,userID = %d", userID);
			itr = vecPeople.erase(itr);
			continue;
		}

		//��ʼ��������Ϣ
		pUser->matchSocre = 0;

		//����ڴ�����
		pUser->userID = userID;
		pUser->money = userData.money;
		pUser->jewels = userData.jewels;
		pUser->userStatus = userData.status;
		pUser->isVirtual = userData.isVirtual;
		memcpy(pUser->name, userData.name, sizeof(pUser->name));
		memcpy(pUser->headURL, userData.headURL, sizeof(pUser->headURL));
		pUser->sex = userData.sex;
		pUser->IsOnline = false;
		pUser->socketIdx = -1;
		memcpy(pUser->longitude, userData.Lng, sizeof(pUser->longitude));
		memcpy(pUser->latitude, userData.Lat, sizeof(pUser->latitude));
		memcpy(pUser->address, userData.address, sizeof(pUser->address));
		memcpy(pUser->motto, userData.motto, sizeof(pUser->motto));

		m_pGameUserManage->AddUser(pUser);

		itr++;
	}

	//�ж���Ч�����������Ƿ�С����Сֵ
	if (vecPeople.size() < (UINT)pMessage->minPeople || vecPeople.size() < m_KernelData.uDeskPeople)
	{
		ERROR_LOG("��ʼʧ�ܣ���������������������������=%d,��ǰ����=%d,matchID=%d", pMessage->minPeople, vecPeople.size(), pMessage->matchID);

		//���ͱ���ʧ���ʼ�֪ͨ���˱����ѣ��������״̬
		for (size_t i = 0; i < vecPeople.size(); i++)
		{
			ClearMatchStatus(MATCH_FAIL_REASON_NOT_ENOUGH_PEOPLE, vecPeople[i].userID, MATCH_TYPE_TIME, pMessage->matchID);
		}

		SendMatchFailMail(MATCH_FAIL_REASON_NOT_ENOUGH_PEOPLE, 0, MATCH_TYPE_TIME, pMessage->matchID);
		ClearMatchUser(pMessage->matchID, vecPeople);

		return false;
	}

	//��ӻ����ˣ����������������
	bool bStartMatchSuccess = true;
	int iNeedRobotCount = vecPeople.size() % m_KernelData.uDeskPeople == 0 ? 0 : (m_KernelData.uDeskPeople - (vecPeople.size() % m_KernelData.uDeskPeople));
	for (int i = 0; i < iNeedRobotCount; i++)
	{
		int userID = MatchGetRobotUserID();
		if (userID <= 0)
		{
			bStartMatchSuccess = false;
			ERROR_LOG("��ӻ�����ʧ��");
			break;
		}

		// �ڴ�����
		GameUserInfo* pUser = new GameUserInfo;
		if (!pUser)
		{
			ERROR_LOG("�����ڴ�ʧ��,userID = %d", userID);
			bStartMatchSuccess = false;
			break;
		}

		//��ʼ��������Ϣ
		pUser->matchSocre = 0;
		MatchUserInfo robotMatchUser;
		robotMatchUser.userID = userID;
		robotMatchUser.byMatchStatus = DESK_MATCH_STATUS_NORMAL;
		robotMatchUser.signUpTime = currTime;
		vecPeople.push_back(robotMatchUser);

		//����ڴ�����
		pUser->userID = userID;
		pUser->money = 0;
		pUser->jewels = 0;
		pUser->userStatus = 0;
		pUser->isVirtual = 1;
		pUser->IsOnline = true;
		pUser->socketIdx = -1;
		RobotPositionInfo positionInfo;
		int iRobotIndex = m_pRedis->GetRobotInfoIndex();
		ConfigManage()->GetRobotPositionInfo(iRobotIndex, positionInfo);
		strcpy(pUser->longitude, positionInfo.longitude.c_str());
		strcpy(pUser->latitude, positionInfo.latitude.c_str());
		strcpy(pUser->address, positionInfo.address.c_str());
		strcpy(pUser->ip, positionInfo.ip.c_str());
		strcpy(pUser->name, positionInfo.name.c_str());
		strcpy(pUser->headURL, positionInfo.headUrl.c_str());
		pUser->sex = positionInfo.sex;

		m_pGameUserManage->AddUser(pUser);
	}
	if (!bStartMatchSuccess)
	{
		//���ͱ���ʧ���ʼ�֪ͨ���˱����ѣ��������״̬
		for (size_t i = 0; i < vecPeople.size() - iNeedRobotCount; i++)
		{
			ClearMatchStatus(MATCH_FAIL_REASON_SYSTEM_ERROR, vecPeople[i].userID, MATCH_TYPE_TIME, pMessage->matchID);
		}

		SendMatchFailMail(MATCH_FAIL_REASON_SYSTEM_ERROR, 0, MATCH_TYPE_TIME, pMessage->matchID);
		ClearMatchUser(pMessage->matchID, vecPeople);

		ERROR_LOG("������ʼʧ�ܣ���ӻ�����ʧ��");
		return false;
	}

	//��ȡ����������
	int iNeedDeskCount = vecPeople.size() / m_KernelData.uDeskPeople;
	std::list<int> matchDeskList;
	if (!GetMatchDesk(iNeedDeskCount, matchDeskList))
	{
		ERROR_LOG("�޷������������������� iNeedDeskCount=%d", iNeedDeskCount);

		//���ͱ���ʧ���ʼ�֪ͨ���˱����ѣ��������״̬
		for (size_t i = 0; i < vecPeople.size() - iNeedRobotCount; i++)
		{
			ClearMatchStatus(MATCH_FAIL_REASON_SYSTEM_ERROR, vecPeople[i].userID, MATCH_TYPE_TIME, pMessage->matchID);
		}

		SendMatchFailMail(MATCH_FAIL_REASON_SYSTEM_ERROR, 0, MATCH_TYPE_TIME, pMessage->matchID);
		ClearMatchUser(pMessage->matchID, vecPeople);

		return false;
	}

	//ȫ����֤ͨ�����Կ�ʼ���������������������
	int iMaxMatchRound = 0;
	for (iMaxMatchRound = 1; iMaxMatchRound <= MAX_MATCH_ROUND + 1; iMaxMatchRound++)
	{
		if (m_KernelData.uDeskPeople * (int)pow(2.0, iMaxMatchRound - 1) > vecPeople.size())
		{
			break;
		}
	}
	iMaxMatchRound = iMaxMatchRound - 1;

	//��ʼ����������
	for (auto itr = matchDeskList.begin(); itr != matchDeskList.end(); itr++)
	{
		CGameDesk * pGameDesk = GetDeskObject(*itr);
		if (pGameDesk == NULL)
		{
			ERROR_LOG("######  ��ʼ����ʧ�ܣ������ڴ��Ҳ���,index=%d  ######", *itr);
			return false;
		}

		pGameDesk->InitDeskMatchData();
		pGameDesk->m_llPartOfMatchID = llPartOfMatchID;
		pGameDesk->m_iCurMatchRound = 1;
		pGameDesk->m_iMaxMatchRound = iMaxMatchRound;
		pGameDesk->m_llStartMatchTime = currTime + 10; //���ÿ�ʼ����ʱ��
	}

	//֪ͨ��Щ��ҹ����μӱ���
	PlatformLoaderNotifyStartMatch notifyMsg;
	notifyMsg.gameID = m_uNameID;
	notifyMsg.matchType = MATCH_TYPE_TIME;
	notifyMsg.matchID = pMessage->matchID;
	notifyMsg.roomID = GetRoomID();
	notifyMsg.peopleCount = vecPeople.size();
	for (int i = 0; i < notifyMsg.peopleCount; i++)
	{
		notifyMsg.userID[i] = vecPeople[i].userID;
	}
	SendMessageToCenterServer(CENTER_MESSAGE_LOADER_NOTIFY_START_MATCH, &notifyMsg, sizeof(notifyMsg));

	//���ñ���״̬Ϊ������
	if (m_pRedisPHP)
	{
		m_pRedisPHP->SetMatchStatus(pMessage->matchID, MATCH_STATUS_PLAYING);
	}

	//�����Ӽ����ڴ�
	m_matchGameDeskMap.insert(std::make_pair(llPartOfMatchID, matchDeskList));

	//��������Ա���ڴ��б���
	m_matchUserMap.insert(std::make_pair(llPartOfMatchID, vecPeople));

	//���������ID
	m_matchMainIDMap[llPartOfMatchID] = pMessage->matchID;

	//�����������
	m_matchTypeMap[llPartOfMatchID] = MATCH_TYPE_TIME;

	//��ʼ����
	AllocDeskStartMatch(matchDeskList, vecPeople);

	return true;
}

//////////////////////////////////////////////////////////////////////
//������

// Ϊ��������һ������������
bool CGameMainManage::GetMatchDesk(int needDeskCount, std::list<int> &listDesk)
{
	if (needDeskCount <= 0)
	{
		ERROR_LOG("��������,needDeskCount=%d", needDeskCount);
		return false;
	}

	listDesk.clear();

	for (unsigned int i = 0; i < m_uDeskCount; i++)
	{
		CGameDesk* pDesk = GetDeskObject(i);
		if (pDesk && !pDesk->IsPlayGame(0) && pDesk->m_llPartOfMatchID == 0)
		{
			listDesk.push_back(i);
		}

		if (listDesk.size() >= (UINT)needDeskCount)
		{
			break;
		}
	}

	if (listDesk.size() != needDeskCount)
	{
		ERROR_LOG("���ӷ������,listDeskCount=%d,needDeskCount=%d", listDesk.size(), needDeskCount);
		return false;
	}

	return true;
}

// Ϊ������Ա�������ӣ�������
void CGameMainManage::AllocDeskStartMatch(const std::list<int> &matchDeskList, const std::vector<MatchUserInfo> &vecPeople)
{
	if (matchDeskList.size() <= 0 || vecPeople.size() <= 0)
	{
		ERROR_LOG("��������ʧ�ܣ�desklistCount=%d,peopleCount=%d", matchDeskList.size(), vecPeople.size());
		return;
	}

	int arrUserID[MAX_MATCH_PEOPLE_COUNT] = { 0 };
	memset(arrUserID, false, sizeof(arrUserID));
	int iRemainMatchPeopleCount = 0;

	for (size_t i = 0; i < vecPeople.size(); i++)
	{
		if (vecPeople[i].userID > 0 && vecPeople[i].byMatchStatus == DESK_MATCH_STATUS_NORMAL)
		{
			arrUserID[iRemainMatchPeopleCount++] = vecPeople[i].userID;
		}
		else
		{
			break;
		}
	}

	if (iRemainMatchPeopleCount / m_KernelData.uDeskPeople != matchDeskList.size())
	{
		ERROR_LOG("####  ʣ����������:iRemainMatchPeopleCount=%d,m_KernelData.uDeskPeople=%d,matchDeskList.size()=%d  ####",
			iRemainMatchPeopleCount, m_KernelData.uDeskPeople, matchDeskList.size());
		return;
	}

	//�����������
	int temp = 0, data_ = 0;
	int iCount = 2;
	while (--iCount >= 0)
	{
		for (int i = 0; i < iRemainMatchPeopleCount; i++)
		{
			temp = CUtil::GetRandNum() % (iRemainMatchPeopleCount - i) + i;
			data_ = arrUserID[temp];
			arrUserID[temp] = arrUserID[i];
			arrUserID[i] = data_;
		}
	}

	for (auto itr = matchDeskList.begin(); itr != matchDeskList.end(); itr++)
	{
		CGameDesk* pDesk = GetDeskObject(*itr);
		if (!pDesk)
		{
			ERROR_LOG("��Ч����������:%d", *itr);
			continue;
		}

		//��������ȡ�������
		for (size_t i = 0; i < m_KernelData.uDeskPeople; i++)
		{
			if (iRemainMatchPeopleCount <= 0)
			{
				ERROR_LOG("#### ʣ���������� ####");
				return;
			}

			int userID = arrUserID[iRemainMatchPeopleCount - 1];

			GameUserInfo* pUser = m_pGameUserManage->GetUser(userID);
			if (!pUser)
			{
				ERROR_LOG("###### �ش����,userID=%d #######", userID);
				return;
			}

			if (!pDesk->UserSitDesk(pUser))
			{
				ERROR_LOG("###### �ش����,userID=%d #######", userID);
				return;
			}

			iRemainMatchPeopleCount--;
		}
	}
}

// ��ʱ�����Կ�ʼ����������
void CGameMainManage::CheckDeskStartMatch()
{
	time_t currTime = time(NULL);

	for (unsigned int i = 0; i < m_uDeskCount; i++)
	{
		CGameDesk* pDesk = GetDeskObject(i);
		if (pDesk &&  pDesk->m_llStartMatchTime > 0 && currTime >= pDesk->m_llStartMatchTime && !pDesk->IsPlayGame(0))
		{
			pDesk->GameBegin(0);
		}
	}
}

// �Ƿ�ȫ�����Ӷ���ɱ���
bool CGameMainManage::IsAllDeskFinishMatch(long long llPartOfMatchID)
{
	std::list<int> &deskList = m_matchGameDeskMap[llPartOfMatchID];

	if (deskList.size() <= 0)
	{
		ERROR_LOG("�ж�һ�ֱ����Ƿ����ʧ�ܣ�desklistCount=%d", deskList.size());
		return false;
	}

	int iFinishCount = 0;
	for (auto itr = deskList.begin(); itr != deskList.end(); itr++)
	{
		CGameDesk* pDesk = GetDeskObject(*itr);
		if (pDesk && pDesk->m_bFinishMatch)
		{
			iFinishCount++;
		}
	}

	if (iFinishCount == deskList.size())
	{
		return true;
	}

	return false;
}

// ĳһ��������Ϸ����
void CGameMainManage::MatchDeskFinish(long long llPartOfMatchID, int deskIdx)
{
	// ֪ͨ���л�û�б���̭����ң�һ��������ɱ���
	LoaderNotifyDeskFinishMatch msg;
	msg.deskIdx = deskIdx;

	std::vector<MatchUserInfo> &vecPeople = m_matchUserMap[llPartOfMatchID];

	for (size_t i = 0; i < vecPeople.size(); i++)
	{
		if (vecPeople[i].userID > 0 && vecPeople[i].byMatchStatus == DESK_MATCH_STATUS_NORMAL)
		{
			SendData(vecPeople[i].userID, &msg, sizeof(msg), MSG_MAIN_LOADER_NOTIFY, MSG_NTF_LOADER_DESK_FINISH_MATCH, 0);
		}
	}
}

// ����������һ��
void CGameMainManage::MatchNextRound(long long llPartOfMatchID, int iCurMatchRound, int iMaxMatchRound)
{
	// ͳ������
	WAUTOCOST("������һ�ֱ�����ʱ��");

	std::vector<MatchUserInfo> &vecPeople = m_matchUserMap[llPartOfMatchID];
	std::list<int> &deskList = m_matchGameDeskMap[llPartOfMatchID];
	BYTE matchType = m_matchTypeMap[llPartOfMatchID];

	//���ݻ�������
	int allUserCount = MatchSortUser(vecPeople, iCurMatchRound);

	//��������������
	int iPromotionCount = 0;
	if (matchType == MATCH_TYPE_PEOPLE || matchType == MATCH_TYPE_TIME && iCurMatchRound > 1)
	{
		iPromotionCount = allUserCount / 2;
	}
	else
	{
		int iCount_ = 0;
		for (int i = 1; i <= MAX_MATCH_ROUND + 1; i++)
		{
			iCount_ = m_KernelData.uDeskPeople * (int)pow(2.0, i - 1);
			if (iCount_ > allUserCount)
			{
				break;
			}
		}

		iPromotionCount = iCount_ / 4;
	}

	LoaderNotifyMatchRank rankmsg;

	//����������������
	rankmsg.peopleCount = allUserCount;
	for (int i = 0; i < allUserCount; i++)
	{
		rankmsg.user[i].userID = vecPeople[i].userID;
		rankmsg.user[i].rank = i + 1;
	}
	int iRankMsgSize = sizeof(LoaderNotifyMatchRank::LoaderNotifyMatchRankUser) * allUserCount + sizeof(rankmsg) - sizeof(rankmsg.user);

	//��̭���
	for (int i = 0; i < allUserCount; i++)
	{
		int userID = vecPeople[i].userID;
		GameUserInfo *pUser = GetUser(userID);
		if (!pUser)
		{
			continue;
		}

		//������뿪����
		CGameDesk* pDesk = GetDeskObject(pUser->deskIdx);
		if (pDesk)
		{
			pDesk->UserLeftDesk(pUser);
		}

		rankmsg.gameID = m_uNameID;
		rankmsg.gameMatchID = m_matchMainIDMap[llPartOfMatchID];
		rankmsg.iCurMatchRound = iCurMatchRound;
		rankmsg.iMaxMatchRound = iMaxMatchRound;

		if (i < iPromotionCount) //�������
		{
			vecPeople[i].byMatchStatus = DESK_MATCH_STATUS_NORMAL;
			rankmsg.type = 1;
			rankmsg.rankMatch = i + 1;

			//��������֪ͨ
			SendData(userID, &rankmsg, iRankMsgSize, MSG_MAIN_LOADER_NOTIFY, MSG_NTF_LOADER_MATCH_RANK, 0);

			//�������1/3��׼��������һ��
			if (pUser->matchSocre > 0)
			{
				pUser->matchSocre /= 3;
			}
			else
			{
				pUser->matchSocre = 0;
			}
		}
		else //��̭���
		{
			vecPeople[i].byMatchStatus = DESK_MATCH_STATUS_FAIL;
			rankmsg.type = 0;
			rankmsg.rankMatch = i + 1;

			//��������֪ͨ
			SendData(userID, &rankmsg, iRankMsgSize, MSG_MAIN_LOADER_NOTIFY, MSG_NTF_LOADER_MATCH_RANK, 0);

			//��������ڴ��redis״̬��Ŀǰ����socket����һ����Լ�����ս
			DelUser(userID);
			m_pRedis->SetUserMatchStatus(userID, MATCH_TYPE_NORMAL, USER_MATCH_STATUS_NORMAL);
			m_pRedis->SetUserMatchRank(userID, (long long)m_uNameID*MAX_GAME_MATCH_ID + rankmsg.gameMatchID, i + 1);

			//�����ʼ�֪ͨ
			SendMatchGiftMail(userID, m_uNameID, matchType, rankmsg.gameMatchID, i + 1);
		}
	}

	//������Ҫ�������������
	int iDeskCount = deskList.size();
	int iClearDeskCount = 0;
	if (matchType == MATCH_TYPE_PEOPLE || matchType == MATCH_TYPE_TIME && iCurMatchRound > 1)
	{
		iClearDeskCount = iDeskCount / 2;
	}
	else
	{
		iClearDeskCount = (allUserCount - iPromotionCount) / m_KernelData.uDeskPeople;
	}

	//�������ӣ���������������
	int iIndex = 0;
	time_t currTime = time(NULL);
	for (auto itr = deskList.begin(); itr != deskList.end(); iIndex++)
	{
		CGameDesk* pDesk = GetDeskObject(*itr);
		if (!pDesk)
		{
			deskList.erase(itr++);
			continue;
		}
		if (iIndex < iClearDeskCount)
		{
			pDesk->InitDeskMatchData();
			deskList.erase(itr++);
			continue;
		}

		pDesk->m_iCurMatchRound++;
		pDesk->m_llStartMatchTime = currTime + 15; //���ÿ�ʼ����ʱ��
		pDesk->m_bFinishMatch = false;

		itr++;
	}

	// ���н��������������
	AllocDeskStartMatch(deskList, vecPeople);
}

// ��������
void CGameMainManage::MatchEnd(long long llPartOfMatchID, int iCurMatchRound, int iMaxMatchRound)
{
	// ͳ������
	WAUTOCOST("����������ʱ��");

	std::vector<MatchUserInfo> &vecPeople = m_matchUserMap[llPartOfMatchID];
	std::list<int> &deskList = m_matchGameDeskMap[llPartOfMatchID];
	BYTE matchType = m_matchTypeMap[llPartOfMatchID];
	int gameMatchID = m_matchMainIDMap[llPartOfMatchID];

	//���ݻ�������
	int allUserCount = MatchSortUser(vecPeople, iCurMatchRound);

	LoaderNotifyMatchRank rankmsg;

	//����������������
	rankmsg.peopleCount = allUserCount;
	for (int i = 0; i < allUserCount; i++)
	{
		rankmsg.user[i].userID = vecPeople[i].userID;
		rankmsg.user[i].rank = i + 1;
	}
	int iRankMsgSize = sizeof(LoaderNotifyMatchRank::LoaderNotifyMatchRankUser) * allUserCount + sizeof(rankmsg) - sizeof(rankmsg.user);

	//������������
	for (int i = 0; i < allUserCount; i++)
	{
		int userID = vecPeople[i].userID;
		GameUserInfo *pUser = GetUser(userID);
		if (!pUser)
		{
			continue;
		}

		//������������֪ͨ
		rankmsg.gameID = m_uNameID;
		rankmsg.gameMatchID = gameMatchID;
		rankmsg.iCurMatchRound = iCurMatchRound;
		rankmsg.iMaxMatchRound = iMaxMatchRound;
		rankmsg.type = 2;
		rankmsg.rankMatch = i + 1;
		SendData(userID, &rankmsg, iRankMsgSize, MSG_MAIN_LOADER_NOTIFY, MSG_NTF_LOADER_MATCH_RANK, 0);

		//������ұ���״̬����������
		m_pRedis->SetUserMatchStatus(userID, MATCH_TYPE_NORMAL, USER_MATCH_STATUS_NORMAL);
		m_pRedis->SetUserMatchRank(userID, (long long)m_uNameID*MAX_GAME_MATCH_ID + rankmsg.gameMatchID, i + 1);

		// �����һ����е�����
		m_pRedis->SetUserRoomID(userID, 0);
		m_pRedis->SetUserDeskIdx(userID, INVALID_DESKIDX);

		// �������������
		m_pGameUserManage->DelUser(userID);

		//�����ʼ�֪ͨ
		SendMatchGiftMail(userID, m_uNameID, matchType, rankmsg.gameMatchID, i + 1);
	}

	//���ñ������������Ϣ
	if (matchType == MATCH_TYPE_TIME)
	{
		//���������������ñ�������
		m_pRedis->SetTimeMatchPeopleRank(gameMatchID, vecPeople);

		//���ñ���״̬Ϊ��������
		if (m_pRedisPHP)
		{
			m_pRedisPHP->SetMatchStatus(gameMatchID, MATCH_STATUS_GAME_OVER);
		}
	}

	//������������
	for (auto itr = deskList.begin(); itr != deskList.end(); itr++)
	{
		CGameDesk* pDesk = GetDeskObject(*itr);
		if (!pDesk)
		{
			continue;
		}

		pDesk->InitDeskMatchData();
	}
	m_matchGameDeskMap.erase(llPartOfMatchID);

	//�������б������
	m_matchUserMap.erase(llPartOfMatchID);

	//���������¼
	m_matchMainIDMap.erase(llPartOfMatchID);

	//�����������
	m_matchTypeMap.erase(llPartOfMatchID);
}

// ���ݻ��֣�����û����̭�����
int CGameMainManage::MatchSortUser(std::vector<MatchUserInfo> &vecPeople, int iCurMatchRound)
{
	int iSortSize = vecPeople.size();
	for (int i = 1; i < iCurMatchRound; i++)
	{
		iSortSize /= 2;
	}

	int iMax = 0;
	for (int i = 0; i < iSortSize - 1; i++)
	{
		iMax = i;
		for (int j = i + 1; j < iSortSize; j++)
		{
			int userID = vecPeople[j].userID;
			int maxUserID = vecPeople[iMax].userID;
			GameUserInfo *pUser = GetUser(userID);
			GameUserInfo *pMaxUser = GetUser(maxUserID);
			if (!pUser || !pMaxUser)
			{
				continue;
			}

			if (pUser->matchSocre > pMaxUser->matchSocre)
			{
				iMax = j;
			}
			else if (pUser->matchSocre == pMaxUser->matchSocre && vecPeople[j].signUpTime < vecPeople[iMax].signUpTime)
			{
				iMax = j;
			}
		}

		if (iMax != i)
		{
			MatchUserInfo byTemp = vecPeople[iMax];
			vecPeople[iMax] = vecPeople[i];
			vecPeople[i] = byTemp;
		}
	}

	return iSortSize;
}

// ���Ż����˼������Ӻ���ұ���
int CGameMainManage::MatchGetRobotUserID()
{
	int userID = 0;

	//��ೢ��500�λ�ȡ
	for (int iCount = 0; iCount < 500; iCount++)
	{
		userID = m_pRedis->GetRobotUserID();
		if (userID <= 0)
		{
			return 0;
		}

		if (m_pGameUserManage->GetUser(userID) != NULL)
		{
			continue;
		}

		UserData userData;
		if (!m_pRedis->GetUserData(userID, userData))
		{
			continue;
		}

		if (userData.isVirtual == 0)
		{
			continue;
		}

		if (userID > 0)
		{
			break;
		}
	}

	return userID;
}

// ���ͱ����ʼ�����
void CGameMainManage::SendMatchGiftMail(int userID, int gameID, BYTE matchType, int gameMatchID, int ranking)
{
	OtherConfig otherConfig;
	if (!m_pRedis->GetOtherConfig(otherConfig))
	{
		otherConfig = ConfigManage()->GetOtherConfig();
	}

	char bufUserID[20] = "";
	sprintf_s(bufUserID, 20, "%d", userID);

	//�������URL
	std::string url = "http://";
	url += otherConfig.http;
	url += "/hm_ucenter/web/index.php?api=match&action=matchAward&userID=";
	url += bufUserID;

	//�����ʼ�֪ͨ
	LoaderAsyncHTTP asyncEvent;
	asyncEvent.userID = userID;
	asyncEvent.postType = HTTP_POST_TYPE_MATCH_GIFT;

	strcpy_s(asyncEvent.url, sizeof(asyncEvent.url), url.c_str());

	m_SQLDataManage.PushLine(&asyncEvent.dataBaseHead, sizeof(LoaderAsyncHTTP), LOADER_ASYNC_EVENT_HTTP, 0, 0);
}

// ���ͱ���ʧ�ܣ��˱����ѣ��Լ��������״̬
void CGameMainManage::SendMatchFailMail(BYTE failReason, int userID, BYTE matchType, int gameMatchID)
{
	OtherConfig otherConfig;
	if (!m_pRedis->GetOtherConfig(otherConfig))
	{
		otherConfig = ConfigManage()->GetOtherConfig();
	}

	// �����ʼ�
	char bufURL[256] = "";

	if (userID == 0)
	{
		sprintf_s(bufURL, sizeof(bufURL), "http://%s/hm_ucenter/web/index.php?api=match&action=matchNotStart&matchID=%d&reason=%d",
			otherConfig.http, gameMatchID, failReason);
	}
	else
	{
		sprintf_s(bufURL, sizeof(bufURL), "http://%s/hm_ucenter/web/index.php?api=match&action=matchFail&userID=%d&matchID=%d&reason=%d",
			otherConfig.http, userID, gameMatchID, failReason);
	}

	LoaderAsyncHTTP asyncEvent;
	asyncEvent.userID = userID == 0 ? gameMatchID : userID;
	asyncEvent.postType = HTTP_POST_TYPE_MATCH_FAIL;
	memcpy(asyncEvent.url, bufURL, min(sizeof(asyncEvent.url), sizeof(bufURL)));
	m_SQLDataManage.PushLine(&asyncEvent.dataBaseHead, sizeof(LoaderAsyncHTTP), LOADER_ASYNC_EVENT_HTTP, 0, 0);
}

// �������״̬
void CGameMainManage::ClearMatchStatus(BYTE failReason, int userID, BYTE matchType, int gameMatchID)
{
	// ������״̬
	m_pRedis->SetUserMatchStatus(userID, MATCH_TYPE_NORMAL, USER_MATCH_STATUS_NORMAL);

	//֪ͨ�����ұ���ʧ��
	PlatformLoaderNotifyStartMatchFail notifyMsg;
	notifyMsg.gameID = m_uNameID;
	notifyMsg.matchType = MATCH_TYPE_TIME;
	notifyMsg.matchID = gameMatchID;
	notifyMsg.reason = failReason;
	SendMessageToCenterServer(CENTER_MESSAGE_LOADER_NOTIFY_START_MATCH_FAIL, &notifyMsg, sizeof(notifyMsg), userID);
}

// �����������ڴ�
void CGameMainManage::ClearMatchUser(int gameMatchID, const std::vector<MatchUserInfo> &vecPeople)
{
	for (size_t i = 0; i < vecPeople.size(); i++)
	{
		m_pGameUserManage->DelUser(vecPeople[i].userID);
	}

	//��ʼ����ʧ�ܣ����ñ���״̬Ϊ��������
	if (m_pRedisPHP)
	{
		m_pRedisPHP->SetMatchStatus(gameMatchID, MATCH_STATUS_GAME_OVER);
	}
}