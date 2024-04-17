#include "pch.h"
#include "ConfigManage.h"
#include "commonuse.h"
#include "Exception.h"
#include <string.h>
#include <WS2tcpip.h>
#include "../curl/curl.h"
#include "KernelDefine.h"
#include "log.h"
#include "MysqlHelper.h"
#include <urlmon.h>
#include <Nb30.h>

#pragma comment(lib,"netapi32.lib") 
#pragma comment(lib, "urlmon.lib")

#ifdef _DEBUG
#pragma comment(lib, "libcurld.lib")
#else // !_DEBUG
#pragma comment(lib, "libcurl.lib")
#endif // _DEBUG

CConfigManage::CConfigManage()
{
	m_pMysqlHelper = new CMysqlHelper;
}

CConfigManage::~CConfigManage()
{
}

CConfigManage* CConfigManage::Instance()
{
	static CConfigManage mgr;
	return &mgr;
}

void CConfigManage::Release()
{
	m_tableFieldDescMap.clear();
	m_gameBaseInfoMap.clear();
	m_roomBaseInfoMap.clear();
	m_buyGameDeskInfoMap.clear();
	m_buyRoomInfoMap.clear();
	m_logonBaseInfoMap.clear();

	curl_global_cleanup();

	if (m_pMysqlHelper)
	{
		delete m_pMysqlHelper;
		m_pMysqlHelper = NULL;
	}

}

bool CConfigManage::Init()
{
	AUTOCOST("CConfigManage::Init()");
	INFO_LOG("configManage Init begin...");

	// ��ʼ��curl
	curl_global_init(CURL_GLOBAL_ALL);

	bool ret = false;

	//// ������֤
	//if (!RequestAuth())
	//{
	//	ERROR_LOG("request auth failed");
	//	AfxMessageBox("��������֤ʧ��", MB_ICONSTOP);
	//	exit(0);
	//}

	// ����db����
	ret = LoadDBConfig();
	if (!ret)
	{
		ERROR_LOG("LoadDBConfig failed");
		return false;
	}

	// �������ķ���������
	ret = LoadCenterServerConfig();
	if (!ret)
	{
		ERROR_LOG("LoadCenterServerConfig failed");
		return false;
	}

	// ���ش�������������
	ret = LoadLogonServerConfig();
	if (!ret)
	{
		ERROR_LOG("LoadLogonServerConfig failed");
		return false;
	}

	// ������Ϸ������
	ret = LoadLoaderServerConfig();
	if (!ret)
	{
		ERROR_LOG("LoadLoaderServerConfig failed");
		return false;
	}

	// ���ع�������
	ret = LoadCommonConfig();
	if (!ret)
	{
		ERROR_LOG("LoadCommonConfig failed");
		return true;
	}

	// �������ݿ�����
	ret = ConnectToDatabase();
	if (!ret)
	{
		ERROR_LOG("ConnectToDatabase failed");
		return false;
	}

	// ���ض�̬����ֶ�����
	ret = LoadTableFiledConfig();
	if (!ret)
	{
		ERROR_LOG("LoadTableFiledConfig failed");
		return false;
	}

	// ���ر������
	ret = LoadTablesPrimaryKey();
	if (!ret)
	{
		ERROR_LOG("LoadTablesPrimaryKey failed");
		return false;
	}

	// ���ػ�������
	ret = LoadBaseConfig();
	if (!ret)
	{
		ERROR_LOG("LoadBaseConfig failed");
		return false;
	}

	//�Ͽ������ݿ������
	m_pMysqlHelper->disconnect();

	INFO_LOG("configManage Init end.");

	return true;
}

bool CConfigManage::LoadDBConfig()
{
	string path = CINIFile::GetAppPath();
	CINIFile file(path + "config.ini");

	string key = TEXT("DB");
	string ret;

	ret = file.GetKeyVal(key, "ip", "127.0.0.1");
	strncpy(m_dbConfig.ip, ret.c_str(), sizeof(m_dbConfig.ip) - 1);

	ret = file.GetKeyVal(key, "user", "sa");
	strncpy(m_dbConfig.user, ret.c_str(), sizeof(m_dbConfig.user) - 1);

	ret = file.GetKeyVal(key, "passwd", "123456");
	strncpy(m_dbConfig.passwd, ret.c_str(), sizeof(m_dbConfig.passwd) - 1);

	ret = file.GetKeyVal(key, "dbName", "HM");
	strncpy(m_dbConfig.dbName, ret.c_str(), sizeof(m_dbConfig.dbName) - 1);

	m_dbConfig.port = file.GetKeyVal(key, "port", 1433);

	return true;
}

bool CConfigManage::LoadRedisConfig()
{
	char sql[128] = "";
	sprintf(sql, "select * from %s", TBL_BASE_REDIS_CONFIG);

	CMysqlHelper::MysqlData dataSet;
	try
	{
		m_pMysqlHelper->queryRecord(sql, dataSet, true);
	}
	catch (MysqlHelper_Exception& excep)
	{
		ERROR_LOG("ִ��sql���ʧ��:%s", excep.errorInfo.c_str());
		return false;
	}

	for (size_t i = 0; i < dataSet.size(); i++)
	{
		RedisConfig config;

		sqlGetValue(dataSet[i], "redisTypeID", config.redisTypeID);
		sqlGetValue(dataSet[i], "ip", config.ip, sizeof(config.ip));
		sqlGetValue(dataSet[i], "port", config.port);
		sqlGetValue(dataSet[i], "passwd", config.passwd, sizeof(config.passwd));

		m_redisConfigMap.insert(std::make_pair(config.redisTypeID, config));
	}

	return true;
}

bool CConfigManage::LoadLoaderServerConfig()
{
	string path = CINIFile::GetAppPath();
	CINIFile file(path + "config.ini");

	string key = TEXT("LOADERSERVER");
	string ret;

	ret = file.GetKeyVal(key, "serviceName", "brnn");
	strncpy(m_loaderServerConfig.serviceName, ret.c_str(), sizeof(m_loaderServerConfig.serviceName) - 1);

	ret = file.GetKeyVal(key, "logonserverPasswd", "e10adc3949ba59abbe56e057f20f883e");
	strncpy(m_loaderServerConfig.logonserverPasswd, ret.c_str(), sizeof(m_loaderServerConfig.logonserverPasswd) - 1);

	ret = file.GetKeyVal(key, "recvThreadNumber", "4");
	m_loaderServerConfig.recvThreadNumber = atoi(ret.c_str());

	return true;
}

bool CConfigManage::LoadCommonConfig()
{
	string path = CINIFile::GetAppPath();
	CINIFile file(path + "config.ini");

	const char* pKey = "COMMON";

	m_commonConfig.logLevel = file.GetKeyVal(pKey, "logLevel", LOG_LEVEL_INFO);
	m_commonConfig.IOCPWorkThreadNumber = file.GetKeyVal(pKey, "IOCPWorkThreadNumber", 4);

	return true;
}

bool CConfigManage::LoadTableFiledConfig()
{
	m_tableFieldDescMap.clear();

	for (size_t i = 0; i < dynamicTbls.size(); i++)
	{
		const char* tblName = dynamicTbls[i];
		if (!tblName)
		{
			continue;
		}

		char buf[MAX_SQL_STATEMENT_SIZE] = "";
		sprintf(buf, "show full fields from %s", tblName);

		CMysqlHelper::MysqlData dataSet;
		try
		{
			m_pMysqlHelper->queryRecord(buf, dataSet);
		}
		catch (MysqlHelper_Exception& excep)
		{
			ERROR_LOG("ִ��sql���ʧ��:%s", excep.errorInfo.c_str());
			return false;
		}

		std::vector<FieldDescriptor> vec;

		for (size_t j = 0; j < dataSet.size(); j++)
		{
			string szType = "";
			int type = FIELD_VALUE_TYPE_NONE;

			szType = dataSet[j]["Type"];

			if (szType.find("int") != string::npos)
			{
				if (szType.find("bigint") != string::npos)
				{
					type = FIELD_VALUE_TYPE_LONGLONG;
				}
				else if (szType.find("tinyint") != string::npos)
				{
					type = FIELD_VALUE_TYPE_CHAR;
				}
				else
				{
					type = FIELD_VALUE_TYPE_INT;
				}
			}
			else if (szType.find("varchar") != string::npos || szType.find("text") != string::npos)
			{
				type = FIELD_VALUE_TYPE_STR;
			}
			else if (szType.find("double") != string::npos)
			{
				type = FIELD_VALUE_TYPE_DOUBLE;
			}

			if (type == FIELD_VALUE_TYPE_NONE)
			{
				// ���ڲ�֧�ֵ���������
				ERROR_LOG("have unsupported type szType=%s", szType);
				return false;
			}

			FieldDescriptor fieldDesc;

			sqlGetValue(dataSet[j], "Field", fieldDesc.fieldName, sizeof(fieldDesc.fieldName));
			fieldDesc.valueType = type;

			vec.push_back(fieldDesc);
		}

		if (vec.size() == 0)
		{
			ERROR_LOG("FieldDescriptor vec is 0 table=%s", tblName);
			return false;
		}

		m_tableFieldDescMap.insert(std::make_pair(std::string(tblName), vec));
	}

	return true;
}

bool CConfigManage::LoadBaseConfig()
{
	AUTOCOST("LoadBaseConfig");

	// ����redis����
	bool ret = LoadRedisConfig();
	if (!ret)
	{
		ERROR_LOG("LoadRedisConfig failed");
		return false;
	}

	ret = LoadGameBaseConfig();
	if (!ret)
	{
		ERROR_LOG("LoadGameBaseConfig failed");
		return false;
	}

	ret = LoadLogonBaseConfig();
	if (!ret)
	{
		ERROR_LOG("LoadLogonBaseConfig failed");
		return false;
	}

	ret = LoadRoomBaseConfig();
	if (!ret)
	{
		ERROR_LOG("LoadRoomBaseConfig failed");
		return false;
	}

	ret = LoadBuyGameDeskConfig();
	if (!ret)
	{
		ERROR_LOG("LoadBuyGameDeskConfig failed");
		return false;
	}

	ret = LoadOtherConfig();
	if (!ret)
	{
		ERROR_LOG("LoadOtherConfig failed");
		return false;
	}

	if (m_serviceType == SERVICE_TYPE_LOADER)
	{
		ret = LoadRobotPositionConfig();
		if (!ret)
		{
			ERROR_LOG("LoadRobotPositionConfig failed");
			return false;
		}
	}

	ret = LoadDirtyWordsConfig();
	if (!ret)
	{
		ERROR_LOG("LoadDirtyWordsConfig failed");
		return false;
	}

	return true;
}

bool CConfigManage::LoadOtherConfig()
{
	char sql[128] = "";
	sprintf(sql, "select * from %s", TBL_BASE_OTHER_CONFIG);

	CMysqlHelper::MysqlData dataSet;
	try
	{
		m_pMysqlHelper->queryRecord(sql, dataSet, true);
	}
	catch (MysqlHelper_Exception& excep)
	{
		ERROR_LOG("ִ��sql���ʧ��:%s", excep.errorInfo.c_str());
		return false;
	}

	for (size_t i = 0; i < dataSet.size(); i++)
	{
		string strKey = "", strValue = "";

		strKey = dataSet[i]["keyConfig"];
		strValue = dataSet[i]["valueConfig"];

		GetOtherConfigKeyValue(strKey, strValue);
	}

	return true;
}

void CConfigManage::SetServiceType(int type)
{
	m_serviceType = type;
}

bool CConfigManage::ConnectToDatabase()
{
	m_pMysqlHelper->init(m_dbConfig.ip, m_dbConfig.user, m_dbConfig.passwd, m_dbConfig.dbName, "", m_dbConfig.port);
	try
	{
		m_pMysqlHelper->connect();
	}
	catch (MysqlHelper_Exception& excep)
	{
		ERROR_LOG("�������ݿ�ʧ��:%s", excep.errorInfo.c_str());
		return false;
	}

	return true;
}

bool CConfigManage::LoadGameBaseConfig()
{
	char sql[128] = "";
	sprintf(sql, "select * from %s", TBL_BASE_GAME);

	CMysqlHelper::MysqlData dataSet;
	try
	{
		m_pMysqlHelper->queryRecord(sql, dataSet, true);
	}
	catch (MysqlHelper_Exception& excep)
	{
		ERROR_LOG("ִ��sql���ʧ��:%s", excep.errorInfo.c_str());
		return false;
	}

	for (size_t i = 0; i < dataSet.size(); i++)
	{
		GameBaseInfo gameInfo;

		sqlGetValue(dataSet[i], "gameID", gameInfo.gameID);
		sqlGetValue(dataSet[i], "name", gameInfo.name, sizeof(gameInfo.name));
		sqlGetValue(dataSet[i], "deskPeople", gameInfo.deskPeople);
		sqlGetValue(dataSet[i], "dllName", gameInfo.dllName, sizeof(gameInfo.dllName));
		sqlGetValue(dataSet[i], "watcherCount", gameInfo.watcherCount);
		sqlGetValue(dataSet[i], "canWatch", gameInfo.canWatch);
		sqlGetValue(dataSet[i], "canCombineDesk", gameInfo.canCombineDesk);
		sqlGetValue(dataSet[i], "multiPeopleGame", gameInfo.multiPeopleGame);

		m_gameBaseInfoMap.insert(std::make_pair(gameInfo.gameID, gameInfo));

	}

	return true;
}

bool CConfigManage::LoadBuyGameDeskConfig()
{
	char sql[128] = "";
	sprintf(sql, "select * from %s", TBL_BASE_BUY_DESK);

	CMysqlHelper::MysqlData dataSet;
	try
	{
		m_pMysqlHelper->queryRecord(sql, dataSet, true);
	}
	catch (MysqlHelper_Exception& excep)
	{
		ERROR_LOG("ִ��sql���ʧ��:%s", excep.errorInfo.c_str());
		return false;
	}

	for (size_t i = 0; i < dataSet.size(); i++)
	{
		BuyGameDeskInfo buyDeskInfo;

		sqlGetValue(dataSet[i], "gameID", buyDeskInfo.gameID);
		sqlGetValue(dataSet[i], "count", buyDeskInfo.count);
		sqlGetValue(dataSet[i], "roomType", buyDeskInfo.roomType);
		sqlGetValue(dataSet[i], "costResType", buyDeskInfo.costResType);
		sqlGetValue(dataSet[i], "costNums", buyDeskInfo.costNums);
		sqlGetValue(dataSet[i], "AAcostNums", buyDeskInfo.AAcostNums);
		sqlGetValue(dataSet[i], "otherCostNums", buyDeskInfo.otherCostNums);
		sqlGetValue(dataSet[i], "peopleCount", buyDeskInfo.peopleCount);

		m_buyGameDeskInfoMap[BuyGameDeskInfoKey(buyDeskInfo.gameID, buyDeskInfo.count, buyDeskInfo.roomType)] = buyDeskInfo;
	}

	return true;
}

bool CConfigManage::LoadRoomBaseConfig()
{
	char sql[128] = "";
	if (m_serviceType == SERVICE_TYPE_LOGON || m_serviceType == SERVICE_TYPE_CENTER)
	{
		sprintf(sql, "select * from %s", TBL_BASE_ROOM);
	}
	else
	{
		sprintf(sql, "select * from %s where serviceName = '%s'", TBL_BASE_ROOM, m_loaderServerConfig.serviceName);
	}

	CMysqlHelper::MysqlData dataSet;
	try
	{
		m_pMysqlHelper->queryRecord(sql, dataSet, true);
	}
	catch (MysqlHelper_Exception& excep)
	{
		ERROR_LOG("ִ��sql���ʧ��:%s", excep.errorInfo.c_str());
		return false;
	}

	for (size_t i = 0; i < dataSet.size(); i++)
	{
		RoomBaseInfo roomBaseInfo;

		sqlGetValue(dataSet[i], "roomID", roomBaseInfo.roomID);
		sqlGetValue(dataSet[i], "gameID", roomBaseInfo.gameID);
		sqlGetValue(dataSet[i], "name", roomBaseInfo.name, sizeof(roomBaseInfo.name));
		sqlGetValue(dataSet[i], "serviceName", roomBaseInfo.serviceName, sizeof(roomBaseInfo.serviceName));
		sqlGetValue(dataSet[i], "type", roomBaseInfo.type);
		sqlGetValue(dataSet[i], "sort", roomBaseInfo.sort);
		sqlGetValue(dataSet[i], "deskCount", roomBaseInfo.deskCount);
		sqlGetValue(dataSet[i], "minPoint", roomBaseInfo.minPoint);
		sqlGetValue(dataSet[i], "maxPoint", roomBaseInfo.maxPoint);
		sqlGetValue(dataSet[i], "basePoint", roomBaseInfo.basePoint);
		sqlGetValue(dataSet[i], "gameBeginCostMoney", roomBaseInfo.gameBeginCostMoney);
		sqlGetValue(dataSet[i], "describe", roomBaseInfo.describe, sizeof(roomBaseInfo.describe));
		sqlGetValue(dataSet[i], "roomSign", roomBaseInfo.roomSign);
		sqlGetValue(dataSet[i], "robotCount", roomBaseInfo.robotCount);
		sqlGetValue(dataSet[i], "level", roomBaseInfo.level);
		sqlGetValue(dataSet[i], "configInfo", roomBaseInfo.configInfo, sizeof(roomBaseInfo.configInfo));

		m_roomBaseInfoMap.insert(std::make_pair(roomBaseInfo.roomID, roomBaseInfo));

		m_buyRoomInfoMap[BuyRoomInfoKey(roomBaseInfo.gameID, roomBaseInfo.type)].push_back(roomBaseInfo.roomID);
	}

	return true;
}

bool CConfigManage::LoadRobotPositionConfig()
{
	AUTOCOST("LoadRobotPositionConfig");

	FILE* fp = fopen("robotPosition.txt", "r+");
	if (!fp)
	{
		return true;
	}

	int iManHeadIndex = MIN_MAN_HEADURL_ID, iWoManHeadIndex = MIN_WOMAN_HEADURL_ID;
	char bufHead[256] = "";

	while (!feof(fp))
	{
		char szBuf[2048] = "";
		fgets(szBuf, sizeof(szBuf), fp);

		std::string src(szBuf);
		RobotPositionInfo info;

		info.ip = ParseJsonValue(src, "ip");
		info.address = ParseJsonValue(src, "address");
		info.latitude = ParseJsonValue(src, "latitude");
		info.longitude = ParseJsonValue(src, "longitude");
		std::string strSex = ParseJsonValue(src, "sex");

		if (strSex == "man")
		{
			info.sex = USER_SEX_MALE;
			sprintf(bufHead, "http://%s/%d.jpg", WEB_ADDRESS, iManHeadIndex);
			iManHeadIndex++;
			if (iManHeadIndex > MAX_MAN_HEADURL_ID)
			{
				iManHeadIndex = MIN_MAN_HEADURL_ID;
			}
		}
		else if (strSex == "woman")
		{
			info.sex = USER_DEX_FEMALE;
			sprintf(bufHead, "http://%s/%d.jpg", WEB_ADDRESS, iWoManHeadIndex);
			iWoManHeadIndex++;
			if (iWoManHeadIndex > MAX_WOMAN_HEADURL_ID)
			{
				iWoManHeadIndex = MIN_WOMAN_HEADURL_ID;
			}
		}
		else
		{
			continue;
		}
		info.headUrl = bufHead;

		std::string name = ParseJsonValue(src, "realName");
		std::string nickName = ParseJsonValue(src, "nickName");
		if (nickName == "")
		{
			nickName = "sdfsdfe";
		}
		int iCount = m_nickName[nickName];
		if (iCount >= 2)
		{
			info.name = name;
		}
		else
		{
			info.name = nickName;
			m_nickName[nickName] ++;
		}

		if (info.ip.size() >= 24 || info.address.size() >= 64 || info.latitude.size() >= 12 || info.longitude.size() >= 12 || info.name.size() >= MAX_USER_NAME_LEN || info.headUrl.size() >= 256)
		{
			ERROR_LOG("invalid robot element ipSize=%d, addressSize=%d latitudeSize=%d longitudeSize=%d", info.ip.size(), info.address.size(), info.latitude.size(), info.longitude.size());
			continue;
		}

		m_robotPositionInfoVec.push_back(info);
	}

	return true;
}

bool CConfigManage::LoadDirtyWordsConfig()
{
	AUTOCOST("LoadDirtyWordsConfig");

	FILE* fp = fopen("dirtywords.txt", "r+");
	if (!fp)
	{
		return true;
	}

	const int dirtyBufSize = 10 * 1024 * 1024;
	char* pBuf = new char[dirtyBufSize];
	if (!pBuf)
	{
		return true;
	}
	memset(pBuf, 0, dirtyBufSize);

	fgets(pBuf, dirtyBufSize, fp);

	int lastSpacePos = -1;
	int begin = -1;
	int end = -1;

	for (size_t i = 0; i < strlen(pBuf); i++)
	{
		if (pBuf[i] == ' ')
		{
			if (lastSpacePos == -1)
			{
				begin = 0;
			}
			else
			{
				begin = lastSpacePos + 1;
			}

			end = i;

			if (end >= begin)
			{
				std::string word = std::string(pBuf + begin, end - begin);
				m_dirtyWordsVec.push_back(word);
			}
			lastSpacePos = i;
		}
	}

	auto iter = m_dirtyWordsVec.begin();
	for (; iter != m_dirtyWordsVec.end();)
	{
		if (*iter == "")
		{
			iter = m_dirtyWordsVec.erase(iter);
		}
		else
		{
			++iter;
		}
	}

	delete[] pBuf;

	return true;
}

bool CConfigManage::LoadTablesPrimaryKey()
{
	for (size_t i = 0; i < dynamicTbls.size(); i++)
	{
		const char* tblName = dynamicTbls[i];
		if (!tblName)
		{
			continue;
		}

		char buf[MAX_SQL_STATEMENT_SIZE] = "";
		sprintf(buf, "SELECT column_name FROM INFORMATION_SCHEMA.`KEY_COLUMN_USAGE` WHERE table_name='%s' AND CONSTRAINT_SCHEMA='%s' AND constraint_name='PRIMARY'", tblName, m_dbConfig.dbName);

		CMysqlHelper::MysqlData dataSet;
		try
		{
			m_pMysqlHelper->queryRecord(buf, dataSet);
		}
		catch (MysqlHelper_Exception& excep)
		{
			ERROR_LOG("ִ��sql���ʧ��:%s", excep.errorInfo.c_str());
			return false;
		}

		if (dataSet.size() > 0)
		{
			char keyName[64] = "";
			sqlGetValue(dataSet[0], "column_name", keyName, sizeof(keyName));
			m_tablePrimaryKeyMap[tblName] = keyName;
		}
	}

	return true;
}

const DBConfig& CConfigManage::GetDBConfig()
{
	return m_dbConfig;
}

const RedisConfig& CConfigManage::GetRedisConfig(int redisTypeID)
{
	return m_redisConfigMap[redisTypeID];
}

const CommonConfig & CConfigManage::GetCommonConfig()
{
	return m_commonConfig;
}

const FtpConfig& CConfigManage::GetFtpConfig()
{
	return m_ftpConfig;
}

int CConfigManage::GetFieldType(const char * tableName, const char * filedName)
{
	if (!tableName || !filedName)
	{
		return FIELD_VALUE_TYPE_NONE;
	}

	// �����ֶΣ�����hash����
	if (!strcmp(filedName, "extendMode"))
	{
		return FIELD_VALUE_TYPE_INT;
	}

	//std::string str(tableName);
	auto iter = m_tableFieldDescMap.find(tableName);
	if (iter == m_tableFieldDescMap.end())
	{
		return -1;
	}

	const auto& vec = iter->second;
	for (int i = 0; i < (int)vec.size(); i++)
	{
		const std::string& field = vec[i].fieldName;
		if (!strcmp(filedName, field.c_str()))
		{
			return vec[i].valueType;
		}
	}

	return FIELD_VALUE_TYPE_NONE;
}

const char * CConfigManage::GetDllFileName(int gameID)
{
	if (gameID <= 0)
	{
		return NULL;
	}

	auto iter = m_gameBaseInfoMap.find(gameID);
	if (iter != m_gameBaseInfoMap.end())
	{
		return iter->second.dllName;
	}

	return NULL;
}

RoomBaseInfo* CConfigManage::GetRoomBaseInfo(int roomID)
{
	if (roomID <= 0)
	{
		return NULL;
	}

	auto iter = m_roomBaseInfoMap.find(roomID);
	if (iter != m_roomBaseInfoMap.end())
	{
		return &iter->second;
	}

	return NULL;
}

bool CConfigManage::GetPrivateRoomIDByGameID(int gameID, std::vector<int> & roomID)
{
	auto iter = m_roomBaseInfoMap.begin();
	for (; iter != m_roomBaseInfoMap.end(); iter++)
	{
		auto info = iter->second;
		if (info.gameID == gameID && (info.type != ROOM_TYPE_GOLD))
		{
			roomID.push_back(info.roomID);
		}
	}

	return true;
}

GameBaseInfo* CConfigManage::GetGameBaseInfo(int gameID)
{
	if (gameID <= 0)
	{
		return NULL;
	}

	auto iter = m_gameBaseInfoMap.find(gameID);
	if (iter != m_gameBaseInfoMap.end())
	{
		return &iter->second;
	}

	return NULL;
}

BuyGameDeskInfo* CConfigManage::GetBuyGameDeskInfo(const BuyGameDeskInfoKey &buyKey)
{
	auto iter = m_buyGameDeskInfoMap.find(buyKey);
	if (iter != m_buyGameDeskInfoMap.end())
	{
		return &iter->second;
	}

	return NULL;
}

const OtherConfig& CConfigManage::GetOtherConfig()
{
	return m_otherConfig;
}

const SendGiftConfig& CConfigManage::GetSendGiftConfig()
{
	return m_sendGiftConfig;
}

const BankConfig& CConfigManage::GetBankConfig()
{
	return m_bankConfig;
}

const FriendsGroupConfig& CConfigManage::GetFriendsGroupConfig()
{
	return m_friendsGroupConfig;
}

bool CConfigManage::GetRobotPositionInfo(int robotID, RobotPositionInfo & info)
{
	if (m_robotPositionInfoVec.size() <= 0)
	{
		// û�������ļ�
		return true;
	}

	int idx = robotID % m_robotPositionInfoVec.size();
	info = m_robotPositionInfoVec[idx];

	return true;
}

const std::vector<FieldDescriptor> & CConfigManage::GetTableFiledDescVec(const std::string& tableName)
{
	static std::vector<FieldDescriptor> vec;

	auto iter = m_tableFieldDescMap.find(tableName);
	if (iter != m_tableFieldDescMap.end())
	{
		return iter->second;
	}

	return vec;
}

const char* CConfigManage::GetTablePriamryKey(const std::string& tableName)
{
	auto iter = m_tablePrimaryKeyMap.find(tableName);
	if (iter == m_tablePrimaryKeyMap.end())
	{
		return NULL;
	}

	return iter->second.c_str();
}

bool CConfigManage::GetInternetIP(char* ip, int size)
{
	if (!ip || size <= 0)
	{
		return false;
	}

	const char* fileName = "c:\\i.ini";
	char buf[2048] = { 0 };    //����ҳ�ж��������ݷ��ڴ˴�

	URLDownloadToFile(0, "https://www.baidu.com/s?wd=IP&rsv_spt=1&rsv_iqid=0xf8f2cb3c005d576b&issp=1&f=8&rsv_bp=1&rsv_idx=2&ie=utf-8&rqlang=cn&tn=baiduhome_pg&rsv_enter=1&oq=ip&inputT=2157&rsv_t=7cecl2BUHQihZNXK9WXnnhO2cQ%2B5S1ENi6lWNwyrcFLcFPYrri3vPYpQUTGtMoeDi%2B5s&rsv_pq=8399d6ff0003ade5&rsv_sug3=6&rsv_sug1=3&rsv_sug7=100&rsv_sug2=0&rsv_sug4=2157", fileName, 0, NULL);
	FILE *fp = fopen(fileName, "r");
	if (!fp)
	{
		return 0;
	}

	const char* tag = "fk=";

	while (!feof(fp))
	{
		memset(buf, 0, sizeof(buf));
		fgets(buf, sizeof(buf) - 1, fp);

		char *c = strstr(buf, tag);
		if (c)
		{
			char* begin = c + strlen(tag) + 1;
			if (*begin != 'I')
			{
				// ��ȡ��ip��ַ
				for (int i = 0; i < size; i++)
				{
					ip[i] = *begin++;
					if (*begin == '\"')
					{
						break;
					}
				}
				break;
			}
		}
	}


	fclose(fp);
	remove(fileName);

	return true;
}

// ��ȡmac��ַ
int CConfigManage::GetMacByNetBIOS(char* mac, int size)
{
	if (!mac || size <= 0)
	{
		return 1000;
	}

	NCB ncb;
	typedef struct _ASTAT_
	{
		ADAPTER_STATUS   adapt;
		NAME_BUFFER   NameBuff[30];
	}ASTAT, *PASTAT;

	ASTAT Adapter;

	typedef struct _LANA_ENUM
	{
		UCHAR   length;
		UCHAR   lana[MAX_LANA];
	}LANA_ENUM;

	LANA_ENUM lana_enum;
	UCHAR uRetCode;
	memset(&ncb, 0, sizeof(ncb));
	memset(&lana_enum, 0, sizeof(lana_enum));
	ncb.ncb_command = NCBENUM;
	ncb.ncb_buffer = (unsigned char *)&lana_enum;
	ncb.ncb_length = sizeof(LANA_ENUM);
	uRetCode = Netbios(&ncb);

	if (uRetCode != NRC_GOODRET)
		return uRetCode;

	for (int lana = 0; lana < lana_enum.length; lana++)
	{
		ncb.ncb_command = NCBRESET;
		ncb.ncb_lana_num = lana_enum.lana[lana];
		uRetCode = Netbios(&ncb);
		if (uRetCode == NRC_GOODRET)
			break;
	}

	if (uRetCode != NRC_GOODRET)
		return uRetCode;

	memset(&ncb, 0, sizeof(ncb));
	ncb.ncb_command = NCBASTAT;
	ncb.ncb_lana_num = lana_enum.lana[0];
	strcpy_s((char*)ncb.ncb_callname, NCBNAMSZ, "*");
	ncb.ncb_buffer = (unsigned char *)&Adapter;
	ncb.ncb_length = sizeof(Adapter);
	uRetCode = Netbios(&ncb);

	if (uRetCode != NRC_GOODRET)
		return uRetCode;

	sprintf_s(mac, size, "%02X-%02X-%02X-%02X-%02X-%02X",
		Adapter.adapt.adapter_address[0],
		Adapter.adapt.adapter_address[1],
		Adapter.adapt.adapter_address[2],
		Adapter.adapt.adapter_address[3],
		Adapter.adapt.adapter_address[4],
		Adapter.adapt.adapter_address[5]);

	return 0;
}

bool CConfigManage::RequestAuth()
{
	AUTOCOST("RequestAuth");

	char myMachineCode[] = "2019-2-23-11-40-55-235"; //ÿ���ں˿�Ψһ����
	char myIP[64] = "";

	if (!GetInternetIP(myIP, sizeof(myIP)))
	{
		// û��Զ��IP
		return true;
	}

	//if (GetMacByNetBIOS(myIP, sizeof(myIP)))
	//{
	//	//��ȡ�����ַʧ��
	//	return true;
	//}

	// ��֤��������Ϣ
	const char* hostName = "api.21poker.cn";
	int port = 80;

	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);

	// ʹ��ԭ������socket����
	SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == INVALID_SOCKET)
	{
		return true;
	}

	HOSTENT* pHostent = gethostbyname(hostName);
	if (!pHostent)
	{
		return true;
	}

	sockaddr_in svrAddr;
	svrAddr.sin_family = AF_INET;
	svrAddr.sin_port = htons(port);
	memcpy(&svrAddr.sin_addr.s_addr, pHostent->h_addr_list[0], pHostent->h_length);

	int ret = connect(sock, (sockaddr*)&svrAddr, sizeof(sockaddr_in));
	if (ret != 0)
	{
		// �Զ˿���û������
		return true;
	}

	// ��װ��Ϣ��
	char msgBody[128] = "";
	sprintf(msgBody, "ip=%s", myIP);

	const char* route = "/apps/checkIp";

	char sendBuf[1024] = "";
	sprintf(sendBuf,
		"POST %s HTTP/1.0\r\n"
		"Host: %s\r\n"
		"Content-type: application/x-www-form-urlencoded\r\n"
		"Content-length: %d\r\n\r\n"
		"%s", route, hostName, strlen(msgBody), msgBody);

	int sendBytes = send(sock, sendBuf, strlen(sendBuf), 0);
	if (sendBytes != strlen(sendBuf))
	{
		return true;
	}

	char recvBuf[1024] = "";

	int recvBytes = recv(sock, recvBuf, sizeof(recvBuf), 0);
	if (recvBytes <= 0)
	{
		// �Զ˹رջ���������Ĭ����֤ͨ����
		return true;
	}

	if (strstr(recvBuf, "\"status\":false") != NULL)
	{
		return false;
	}

	return true;
}

std::string CConfigManage::ParseJsonValue(const std::string& src, const char* key)
{
	if (src == "")
	{
		return "";
	}

	char strKey[128] = "";
	sprintf(strKey, "\"%s\":", key);

	int pos = src.find(strKey);
	int begin = pos + strlen(strKey);
	std::string subStr = src.substr(begin);

	int realBegin = -1;
	int realEnd = -1;
	for (size_t i = 0; i < subStr.length(); i++)
	{
		char c = subStr[i];
		if (c == '"')
		{
			if (realBegin == -1)
			{
				realBegin = i;
			}
			else if (realEnd == -1)
			{
				realEnd = i;
				break;
			}
		}
	}

	std::string value(subStr.c_str() + realBegin + 1, subStr.c_str() + realEnd);
	return value;
}

void CConfigManage::GetOtherConfigKeyValue(std::string &strKey, std::string &strValue)
{
	//// �������
	if (strKey == "supportTimesEveryDay")
	{
		m_otherConfig.supportTimesEveryDay = atoi(strValue.c_str());
	}
	else if (strKey == "supportMinLimitMoney")
	{
		m_otherConfig.supportMinLimitMoney = atoi(strValue.c_str());
	}
	else if (strKey == "supportMoneyCount")
	{
		m_otherConfig.supportMoneyCount = atoi(strValue.c_str());
	}

	//// ע���������
	else if (strKey == "registerGiveMoney")
	{
		m_otherConfig.registerGiveMoney = atoi(strValue.c_str());
	}
	else if (strKey == "registerGiveJewels")
	{
		m_otherConfig.registerGiveJewels = atoi(strValue.c_str());
	}

	//// ÿ���¼���ͽ�����
	else if (strKey == "logonGiveMoneyEveryDay")
	{
		m_otherConfig.logonGiveMoneyEveryDay = atoi(strValue.c_str());
	}

	//// ����㲥���
	else if (strKey == "sendHornCostJewels")
	{
		m_otherConfig.sendHornCostJewels = atoi(strValue.c_str());
	}

	//// ħ���������
	else if (strKey == "useMagicExpressCostMoney")
	{
		m_otherConfig.useMagicExpressCostMoney = atoi(strValue.c_str());
	}

	//// ���Ѵ���
	else if (strKey == "friendRewardMoney")
	{
		m_otherConfig.friendRewardMoney = atoi(strValue.c_str());
	}
	else if (strKey == "friendRewardMoneyCount")
	{
		m_otherConfig.friendRewardMoneyCount = atoi(strValue.c_str());
	}
	else if (strKey == "friendTakeRewardMoneyCount")
	{
		m_otherConfig.friendTakeRewardMoneyCount = atoi(strValue.c_str());
	}

	//// �����б�����
	else if (strKey == "buyingDeskCount")
	{
		m_otherConfig.buyingDeskCount = atoi(strValue.c_str());
	}

	//// ת�����
	else if (strKey == "sendGiftMyLimitMoney")
	{
		m_sendGiftConfig.myLimitMoney = _atoi64(strValue.c_str());
	}
	else if (strKey == "sendGiftMyLimitJewels")
	{
		m_sendGiftConfig.myLimitJewels = atoi(strValue.c_str());
	}
	else if (strKey == "sendGiftMinMoney")
	{
		m_sendGiftConfig.sendMinMoney = _atoi64(strValue.c_str());
	}
	else if (strKey == "sendGiftMinJewels")
	{
		m_sendGiftConfig.sendMinJewels = atoi(strValue.c_str());
	}
	else if (strKey == "sendGiftRate")
	{
		m_sendGiftConfig.rate = atof(strValue.c_str());
	}

	//// �������
	else if (strKey == "bankMinSaveMoney")
	{
		m_bankConfig.minSaveMoney = _atoi64(strValue.c_str());
	}
	else if (strKey == "bankSaveMoneyMuti")
	{
		m_bankConfig.saveMoneyMuti = atoi(strValue.c_str());
	}
	else if (strKey == "bankMinTakeMoney")
	{
		m_bankConfig.minTakeMoney = _atoi64(strValue.c_str());
	}
	else if (strKey == "bankTakeMoneyMuti")
	{
		m_bankConfig.takeMoneyMuti = atoi(strValue.c_str());
	}
	else if (strKey == "bankMinTransfer")
	{
		m_bankConfig.minTransferMoney = _atoi64(strValue.c_str());
	}
	else if (strKey == "bankTransferMuti")
	{
		m_bankConfig.transferMoneyMuti = atoi(strValue.c_str());
	}

	//// ���ֲ����
	else if (strKey == "groupCreateCount")
	{
		m_friendsGroupConfig.groupCreateCount = atoi(strValue.c_str());
	}
	else if (strKey == "groupMemberCount")
	{
		m_friendsGroupConfig.groupMemberCount = atoi(strValue.c_str());
	}
	else if (strKey == "groupJoinCount")
	{
		m_friendsGroupConfig.groupJoinCount = atoi(strValue.c_str());
	}
	else if (strKey == "groupManageRoomCount")
	{
		m_friendsGroupConfig.groupManageRoomCount = atoi(strValue.c_str());
	}
	else if (strKey == "groupRoomCount")
	{
		m_friendsGroupConfig.groupRoomCount = atoi(strValue.c_str());
	}
	else if (strKey == "groupAllAlterNameCount")
	{
		m_friendsGroupConfig.groupAllAlterNameCount = atoi(strValue.c_str());
	}
	else if (strKey == "groupEveAlterNameCount")
	{
		m_friendsGroupConfig.groupEveAlterNameCount = atoi(strValue.c_str());
	}
	else if (strKey == "groupTransferCount")
	{
		m_friendsGroupConfig.groupTransferCount = atoi(strValue.c_str());
	}

	////ftp���������
	else if (strKey == "ftpIP")
	{
		memcpy(m_ftpConfig.ftpIP, strValue.c_str(), min(strlen(strValue.c_str()), sizeof(m_ftpConfig.ftpIP) - 1));
	}
	else if (strKey == "ftpUser")
	{
		memcpy(m_ftpConfig.ftpUser, strValue.c_str(), min(strlen(strValue.c_str()), sizeof(m_ftpConfig.ftpUser) - 1));
	}
	else if (strKey == "ftpPasswd")
	{
		memcpy(m_ftpConfig.ftpPasswd, strValue.c_str(), min(strlen(strValue.c_str()), sizeof(m_ftpConfig.ftpPasswd) - 1));
	}

	//// ipע������
	else if (strKey == "byIsIPRegisterLimit")
	{
		m_otherConfig.byIsIPRegisterLimit = atoi(strValue.c_str());
	}
	else if (strKey == "IPRegisterLimitCount")
	{
		m_otherConfig.IPRegisterLimitCount = atoi(strValue.c_str());
	}

	//// �ֲ�ʽ����
	else if (strKey == "byIsDistributedTable")
	{
		m_otherConfig.byIsDistributedTable = atoi(strValue.c_str());
	}

	//// http����
	else if (strKey == "http")
	{
		memcpy(m_otherConfig.http, strValue.c_str(), min(strlen(strValue.c_str()), sizeof(m_otherConfig.http) - 1));
	}

	//// ����1��1ƽ̨
	else if (strKey == "byIsOneToOne")
	{
		m_otherConfig.byIsOneToOne = atoi(strValue.c_str());
	}
}

bool CConfigManage::LoadLogonServerConfig()
{
	string path = CINIFile::GetAppPath();
	CINIFile file(path + "config.ini");

	string key = TEXT("LOGONSERVER");

	m_logonServerConfig.logonID = file.GetKeyVal(key, "logonID", 1);

	return true;
}

bool CConfigManage::LoadCenterServerConfig()
{
	string path = CINIFile::GetAppPath();
	CINIFile file(path + "config.ini");

	string key = TEXT("CENTERSERVER");
	string ret;

	ret = file.GetKeyVal(key, "ip", "127.0.0.1");
	strncpy(m_centerServerConfig.ip, ret.c_str(), sizeof(m_centerServerConfig.ip) - 1);

	m_centerServerConfig.port = file.GetKeyVal(key, "port", 3016);
	m_centerServerConfig.maxSocketCount = file.GetKeyVal(key, "maxSocketCount", 200);

	return true;
}

const LogonServerConfig & CConfigManage::GetLogonServerConfig()
{
	return m_logonServerConfig;
}

const CenterServerConfig & CConfigManage::GetCenterServerConfig()
{
	return m_centerServerConfig;
}

// ��������������
bool CConfigManage::LoadLogonBaseConfig()
{
	char sql[128] = "";
	sprintf(sql, "select * from %s", TBL_BASE_LOGON);

	CMysqlHelper::MysqlData dataSet;
	try
	{
		m_pMysqlHelper->queryRecord(sql, dataSet, true);
	}
	catch (MysqlHelper_Exception& excep)
	{
		ERROR_LOG("ִ��sql���ʧ��:%s", excep.errorInfo.c_str());
		return false;
	}

	for (size_t i = 0; i < dataSet.size(); i++)
	{
		LogonBaseInfo logonBaseInfo;

		sqlGetValue(dataSet[i], "logonID", logonBaseInfo.logonID);
		sqlGetValue(dataSet[i], "ip", logonBaseInfo.ip, sizeof(logonBaseInfo.ip));
		sqlGetValue(dataSet[i], "intranetIP", logonBaseInfo.intranetIP, sizeof(logonBaseInfo.intranetIP));
		sqlGetValue(dataSet[i], "port", logonBaseInfo.port);
		sqlGetValue(dataSet[i], "maxPeople", logonBaseInfo.maxPeople);

		ServerConfigInfo configInfo(logonBaseInfo.port, logonBaseInfo.ip);

		if (m_logonPortSet.count(configInfo) > 0)
		{
			ERROR_LOG("��½���˿ڻ�IP�ظ�");
			return false;
		}

		m_logonPortSet.insert(configInfo);
		m_logonBaseInfoMap.insert(std::make_pair(logonBaseInfo.logonID, logonBaseInfo));
	}

	if (m_logonBaseInfoMap.size() > MAX_LOGON_SERVER_COUNT)
	{
		ERROR_LOG("���õĵ�½���������ࡣ��%d/%d��", m_logonBaseInfoMap.size(), MAX_LOGON_SERVER_COUNT);
		return false;
	}

	if (m_logonBaseInfoMap.size() == 0)
	{
		ERROR_LOG("û�����õ�½������%d/%d��", m_logonBaseInfoMap.size(), MAX_LOGON_SERVER_COUNT);
		return false;
	}

	return true;
}

// ��ȡ������������
LogonBaseInfo* CConfigManage::GetLogonBaseInfo(int logonID)
{
	if (logonID <= 0)
	{
		return NULL;
	}

	auto iter = m_logonBaseInfoMap.find(logonID);
	if (iter != m_logonBaseInfoMap.end())
	{
		return &iter->second;
	}

	return NULL;
}

// ��ȡ����roomID��Ϣ
void CConfigManage::GetBuyRoomInfo(int gameID, int roomType, std::vector<int> &roomIDVec)
{
	roomIDVec.clear();

	auto iter = m_buyRoomInfoMap.find(BuyRoomInfoKey(gameID, roomType));
	if (iter != m_buyRoomInfoMap.end())
	{
		roomIDVec = iter->second;
	}
}

// ��÷ֱ�ı���
bool CConfigManage::GetTableNameByDate(const char * name, char * dateName, size_t size)
{
	if (name == NULL || dateName == NULL || size <= 1)
	{
		return false;
	}

	if (!m_otherConfig.byIsDistributedTable)
	{
		sprintf_s(dateName, size, "%s", name);

		return true;
	}

	SYSTEMTIME sys;
	GetLocalTime(&sys);

	sprintf_s(dateName, size, "%s_%d%02d", name, sys.wYear, sys.wMonth);

	return true;
}

bool CConfigManage::sqlGetValue(std::map<string, string> & data, const char * szFieldName, int &iValue)
{
	if (!szFieldName || data.size() <= 0)
	{
		return false;
	}

	iValue = atoi(data[szFieldName].c_str());

	return true;
}

bool CConfigManage::sqlGetValue(std::map<string, string> & data, const char * szFieldName, UINT &uValue)
{
	if (!szFieldName || data.size() <= 0)
	{
		return false;
	}

	uValue = atoi(data[szFieldName].c_str());

	return true;
}

bool CConfigManage::sqlGetValue(std::map<string, string> & data, const char * szFieldName, long &lValue)
{
	if (!szFieldName || data.size() <= 0)
	{
		return false;
	}

	lValue = atol(data[szFieldName].c_str());

	return true;
}

bool CConfigManage::sqlGetValue(std::map<string, string> & data, const char * szFieldName, long long &llValue)
{
	if (!szFieldName || data.size() <= 0)
	{
		return false;
	}

	llValue = _atoi64(data[szFieldName].c_str());

	return true;
}

bool CConfigManage::sqlGetValue(std::map<string, string> & data, const char * szFieldName, double &dValue)
{
	if (!szFieldName || data.size() <= 0)
	{
		return false;
	}

	dValue = atof(data[szFieldName].c_str());

	return true;
}

bool CConfigManage::sqlGetValue(std::map<string, string> & data, const char * szFieldName, bool &bValue)
{
	if (!szFieldName || data.size() <= 0)
	{
		return false;
	}

	bValue = atoi(data[szFieldName].c_str()) != 0;

	return true;
}

bool CConfigManage::sqlGetValue(std::map<string, string> & data, const char * szFieldName, BYTE &dValue)
{
	if (!szFieldName || data.size() <= 0)
	{
		return false;
	}

	dValue = (BYTE)atoi(data[szFieldName].c_str());

	return true;
}

bool CConfigManage::sqlGetValue(std::map<string, string> & data, const char * szFieldName, char szBuffer[], UINT uSize)
{
	if (!szFieldName || !szBuffer || data.size() <= 0 || uSize <= 1)
	{
		return false;
	}

	memcpy(szBuffer, data[szFieldName].c_str(), min(data[szFieldName].size(), uSize - 1));

	return true;
}