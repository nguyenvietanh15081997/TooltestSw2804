#include "BleProtocol.h"
#include <stdlib.h>
#include <thread>
#include <functional>
#include <byteswap.h>
#include "Log.h"
#include "Util.h"
#include <string.h>
#include <algorithm>
#include "BleOpCode.h"
#include "AES.h"

BleProtocol *bleProtocol = NULL;

static uint8_t keyAes[] = {0x44, 0x69, 0x67, 0x69, 0x74, 0x61, 0x6c, 0x40, 0x32, 0x38, 0x31, 0x31, 0x32, 0x38, 0x30, 0x34};
static uint8_t plaintext[] = {0x24, 0x02, 0x28, 0x04, 0x28, 0x11, 0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static uint8_t appKeyDefault[] = {0x60, 0x96, 0x47, 0x71, 0x73, 0x4f, 0xbd, 0x76, 0xe3, 0xb4, 0x05, 0x19, 0xd1, 0xd9, 0x4a, 0x48};

BleProtocol::BleProtocol(char *uartPort, int baudrate) : Uart(uartPort, baudrate, 100000)
{
	nextAddr = 0;
	haveNewMac = false;
	haveGetMacRsp = true;
	isProvisioning = false;
	isInitKey = false;

	this->ble_addr = 1;
	this->ble_iv_index = 0x11223344;
	this->ble_netkey = "28042804-2804-2804-2804-280428042804";
	this->ble_appkey = "60964771-734f-bd76-e3b4-0519d1d94a48";
	this->ble_devicekey = "20242024-2024-2024-2024-202420242024";

	this->addrDevTesting = 0;
}

BleProtocol::~BleProtocol()
{
}

uint16_t BleProtocol::getBleAddr()
{
	return ble_addr;
}

uint32_t BleProtocol::getBleIvIndex()
{
	return ble_iv_index;
}

string BleProtocol::getBleNetKey()
{
	return ble_netkey;
}

string BleProtocol::getBleAppKey()
{
	return ble_appkey;
}

string BleProtocol::getBleDeviceKey()
{
	return ble_devicekey;
}

uint16_t BleProtocol::getAddrDevTesting()
{
	return addrDevTesting;
}

void BleProtocol::setBleAddr(uint16_t addr)
{
	this->ble_addr = addr;
}

void BleProtocol::setBleIvIndex(uint32_t ivIndex)
{
	this->ble_iv_index = ivIndex;
}

void BleProtocol::setBleNetkey(string netkey)
{
	this->ble_netkey = netkey;
}

void BleProtocol::setBleAppkey(string appkey)
{
	this->ble_appkey = appkey;
}

void BleProtocol::setBleDevicekey(string devicekey)
{
	this->ble_devicekey = devicekey;
}

void BleProtocol::setAddrDevTesting(uint16_t addrDev)
{
	this->addrDevTesting = addrDev;
}

static void AddDeviceThread(void *data)
{
	BleProtocol *bleProtocol = (BleProtocol *)data;
	int timeout = 0;
	while (1)
	{
		if (bleProtocol->IsProvision())
		{
			scan_device_message_t scan_device_message;
			if (bleProtocol->haveNewMac)
			{
				timeout = 0;
				memcpy(&scan_device_message, &bleProtocol->scanDeviceMessage, sizeof(scan_device_message_t));
				bleProtocol->AddDevice(&scan_device_message);
				bleProtocol->haveGetMacRsp = true;
				bleProtocol->haveNewMac = false;
			}
			else
			{
				timeout++;
				if (timeout >= 300)
				{
					timeout = 0;
					bleProtocol->SetProvisioning(false);
					bleProtocol->StopScan();
					string cmdStop = "{\"cmd\":\"stopScanBle\",\"data\":{\"code\":0}}";
					// gateway->LocalPublish(cmdStop);
				}
			}
		}
		usleep(100000);
	}
}

static void HandleOpcodeBle(void *data)
{
	BleProtocol *bleProtocol = (BleProtocol *)data;
	message_rsp_st *message_rsp = NULL;
	while (1)
	{
		if (bleProtocol->GetOpcodeExceptionMessage(&message_rsp) == CODE_OK)
		{
			bleProtocol->CheckOpcodeException(message_rsp);
			free(message_rsp);
		}
		usleep(100000);
	}
}

void BleProtocol::init()
{
	Uart::init();
	thread addDeviceThreadThread(AddDeviceThread, this);
	addDeviceThreadThread.detach();
	thread handleOpcodeBleThread(HandleOpcodeBle, this);
	handleOpcodeBleThread.detach();
	usleep(100000); // wait for thread start
}

void BleProtocol::InitKey()
{
	while (GetNetKey())
	{
		sleep(4);
	}
	CheckKeyBle();
	isInitKey = true;
}

void BleProtocol::CheckKeyBle()
{
	string bleAppkey = getBleAppKey();
	string bleNetkey = getBleNetKey();
	string bleDevicekey = getBleDeviceKey();
	srand(time(NULL));

	string netkeyStr = Util::uuidToStr((uint8_t *)netKey);

	if (getBleAppKey() != "")
	{
		string tempAppKey = bleAppkey;
		LOGD("Appkey: %s", tempAppKey.c_str());
		tempAppKey.erase(std::remove(tempAppKey.begin(), tempAppKey.end(), '-'), tempAppKey.end());
		if (tempAppKey.size() == 32)
		{
			for (int i = 0; i < tempAppKey.length(); i += 2)
			{
				std::string hexByte = tempAppKey.substr(i, 2);
				appKey[i / 2] = std::stoi(hexByte, nullptr, 16);
			}
		}
	}

	if (bleNetkey != netkeyStr)
	{
		ResetBle();

		if (bleNetkey != "")
		{
			string tempNetKey = bleNetkey;
			LOGD("Netkey: %s", tempNetKey.c_str());
			tempNetKey.erase(std::remove(tempNetKey.begin(), tempNetKey.end(), '-'), tempNetKey.end());
			if (tempNetKey.size() == 32)
			{
				for (int i = 0; i < tempNetKey.length(); i += 2)
				{
					std::string hexByte = tempNetKey.substr(i, 2);
					netKey[i / 2] = std::stoi(hexByte, nullptr, 16);
				}
			}
		}
		SetNetKey();

		if (bleDevicekey != "")
		{
			string tempDeviceKey = bleDevicekey;
			LOGD("Gwkey: %s", tempDeviceKey.c_str());
			tempDeviceKey.erase(std::remove(tempDeviceKey.begin(), tempDeviceKey.end(), '-'), tempDeviceKey.end());
			if (tempDeviceKey.size() == 32)
			{
				for (int i = 0; i < tempDeviceKey.length(); i += 2)
				{
					std::string hexByte = tempDeviceKey.substr(i, 2);
					gwKey[i / 2] = std::stoi(hexByte, nullptr, 16);
				}
			}
		}
		SetGwKey();

		if (bleAppkey != "")
		{
			string tempAppKey = bleAppkey;
			LOGD("Appkey: %s", tempAppKey.c_str());
			tempAppKey.erase(std::remove(tempAppKey.begin(), tempAppKey.end(), '-'), tempAppKey.end());
			if (tempAppKey.size() == 32)
			{
				for (int i = 0; i < tempAppKey.length(); i += 2)
				{
					std::string hexByte = tempAppKey.substr(i, 2);
					appKey[i / 2] = std::stoi(hexByte, nullptr, 16);
				}
			}
		}
		UpdateAppKey(bleAppkey);
	}
}

int BleProtocol::GetOpcodeExceptionMessage(message_rsp_st **data)
{
	int rs = CODE_ERROR;
	vectorCheckOpcodeMtx.lock();
	if (messageCheckOpcodeList.size() > 0)
	{
		*data = messageCheckOpcodeList[0];
		messageCheckOpcodeList.erase(messageCheckOpcodeList.begin());
		rs = CODE_OK;
	}
	vectorCheckOpcodeMtx.unlock();
	return rs;
}

void BleProtocol::CheckOpcodeException(message_rsp_st *message_rsp)
{
	// LOGD("CheckOpcodeException");
	switch (message_rsp->opcode)
	{
	case HCI_GATEWAY_CMD_UPDATE_MAC:
		if (IsProvision() && !haveNewMac)
		{
			memcpy(&scanDeviceMessage, message_rsp->data, sizeof(scan_device_message_t));
			haveNewMac = true;
			haveGetMacRsp = false;
		}
		break;

	case HCI_GATEWAY_RSP_OP_CODE:
	{
		typedef struct __attribute__((packed))
		{
			uint16_t dev_addr;
			uint16_t gw_addr;
			uint8_t data[100];
		} data_message_t;
		data_message_t *data_message = (data_message_t *)message_rsp->data;
		uint16_t opcode = data_message->data[0] | (data_message->data[1] << 8);
		uint16_t header = data_message->data[3] | (data_message->data[4] << 8);
		setAddrDevTesting(data_message->dev_addr);
		break;
	}

	case HCI_GATEWAY_CMD_SEND_NODE_INFO:
	{
		for (int i = 0; i < 16; i++)
		{
			deviceKey[i] = message_rsp->data[i + 4];
		}
		break;
	}
	default:
		break;
	}
}

int BleProtocol::OnMessage(unsigned char *data, int len)
{
	// LOGD("OnMessage len: %d", len);
	uint8_t *d = data;
	int l = len;
	message_rsp_st *message_rsp = NULL;
	message_rsp_st *old_message_rsp = NULL;
	bool is_dupplicate = false;
	bool match;
	Util::LedBle(false);
	Util::LedServiceLock();
	while (l >= 5)
	{
		message_rsp = (message_rsp_st *)d;
		if (message_rsp->len >= 3 && message_rsp->len <= 36)
		{
			if (message_rsp->magic == 0x80 ||
				message_rsp->magic == 0x90 ||
				message_rsp->magic == 0x91 ||
				message_rsp->magic == 0x92 ||
				message_rsp->magic == 0xfa)
			{
				if (haveGetMacRsp || (!haveGetMacRsp && message_rsp->opcode != HCI_GATEWAY_CMD_UPDATE_MAC))
				{
					uint16_t packageLen = message_rsp->len + 2;
					is_dupplicate = false;
					if (old_message_rsp && message_rsp->len == old_message_rsp->len)
					{
						is_dupplicate = true;
						for (int i = 0; i < message_rsp->len; i++)
						{
							if (message_rsp->data[i] != old_message_rsp->data[i])
							{
								is_dupplicate = false;
								break;
							}
						}
					}
					if (!is_dupplicate)
					{
						if (message_rsp->len >= 3 && message_rsp->len <= l - 2)
						{
							// LOGD("onMessage opcode: 0x%02X, len: %d", message_rsp->opcode, message_rsp->len);
							for (auto &messageResp : messageRespList)
							{
								if (message_rsp->opcode == messageResp->opcode)
								{
									match = true;
									if (messageResp->compare_data)
									{
										for (int i = 0; i < messageResp->compare_len; i++)
										{
											if (message_rsp->data[messageResp->compare_position + i] != messageResp->compare_data[i])
												match = false;
										}
									}
									if (match)
									{
										messageResp->status = true;
										if (messageResp->len)
										{
											*(messageResp->len) = message_rsp->len - 2;
											if (messageResp->data)
												memcpy(messageResp->data, message_rsp->data, *messageResp->len);
										}
									}
								}
							}
							vectorCheckOpcodeMtx.lock();
							if (messageCheckOpcodeList.size() < BLE_CHECK_OPCODE_BUFFER_MAX_SIZE)
							{
								message_rsp_st *messageCheckOpcode = (message_rsp_st *)malloc(packageLen);
								memcpy(messageCheckOpcode, message_rsp, packageLen);
								messageCheckOpcodeList.push_back(messageCheckOpcode);
							}
							vectorCheckOpcodeMtx.unlock();
						}
						else if (message_rsp->len < 3 || message_rsp->len > 36)
						{
							LOGW("Wrong uart data");
							l = 0;
							break;
						}
						else
						{
							break;
						}
					}
					old_message_rsp = message_rsp;
					l -= packageLen;
					d += packageLen;
				}
				else
				{
					d++;
					l--;
				}
			}
			else
			{
				d++;
				l--;
			}
		}
		else
		{
			d++;
			l--;
		}
		usleep(300);
	}
	Util::LedBle(true);
	Util::LedServiceUnlock();
	return l;
}

int BleProtocol::SendMessage(uint16_t opReq, uint8_t *dataReq, int lenReq, uint8_t opRsp, uint8_t *dataRsp, int *lenRsp, uint32_t timeout, uint8_t *compare_data, int compare_position, int compare_len)
{
	mtxWaitSendUart.lock();
	int rs = CODE_OK;
	message_rsp_list_st message_rsp_list = {
		.status = false,
		.opcode = opRsp,
		.len = lenRsp,
		.data = dataRsp,
		.compare_data = compare_data,
		.compare_position = compare_position,
		.compare_len = compare_len,
	};
	if (opRsp)
	{
		// TODO: add mutex
		messageRespList.push_back(&message_rsp_list);
	}
	message_req_st message_req = {
		.opcode = opReq,
	};
	for (int i = 0; i < lenReq; i++)
	{
		message_req.data[i] = dataReq[i];
	}
	Write((uint8_t *)&message_req, lenReq + 2);
	if (opRsp)
	{
		while (!message_rsp_list.status && timeout--)
		{
			usleep(1000);
		}
		if (!message_rsp_list.status)
		{
			rs = CODE_ERROR;
		}
		messageRespList.erase(remove(messageRespList.begin(), messageRespList.end(), &message_rsp_list), messageRespList.end());
	}
	else
	{
		usleep(1000 * timeout);
	}
	mtxWaitSendUart.unlock();
	return rs;
	// return Write(dataReq, lenReq);
}

int BleProtocol::GetNetKey()
{
	LOGD("GetNetKey");
	uint8_t d = HCI_GATEWAY_CMD_GET_PRO_SELF_STS;
	uint8_t dataRsp[100];
	int lenRsp;
	int rs = SendMessage(SYSTEM_REQ, &d, 1, HCI_GATEWAY_CMD_PRO_STS_RSP, dataRsp, &lenRsp, 5000);
	if (rs == CODE_OK)
	{
		// pro_net_info = (pro_net_info_t *)&dataRsp[1];
		memcpy(&pro_net_info.netKey[0], &dataRsp[1], sizeof(pro_net_info_t));

		uint32_t ivIndex = bswap_32(pro_net_info.iv_index);
		if ((ivIndex == 0x11223344))
		{
			for (int i = 0; i < 16; i++)
			{
				netKey[i] = pro_net_info.netKey[i];
			}

			nextAddr = pro_net_info.unicast_address;
			if (nextAddr == 0)
				nextAddr = 2;
			LOGW("nextAddr: 0x%04X - %d", nextAddr, nextAddr);
		}
		else
		{
			srand((int)time(0));
			for (int i = 0; i < 16; i++)
			{
				netKey[i] = rand() % 256;
			}
			string netkeyStr = Util::uuidToStr((uint8_t *)netKey);
		}
	}
	else
	{
		LOGE("Send GetNetKey error, rs: %d", rs);
		rs = CODE_ERROR;
	}
	return rs;
}

int BleProtocol::SetNetKey()
{
	LOGD("SetNetKey");
	typedef struct __attribute__((packed))
	{
		uint8_t opcode;
		uint8_t netKey[16];
		uint8_t rev[3];
		uint32_t magic;
		uint16_t addr;
	} set_netkey_message_t;
	set_netkey_message_t set_netkey_message;
	memset(&set_netkey_message, 0x00, sizeof(set_netkey_message));
	set_netkey_message.opcode = HCI_GATEWAY_CMD_SET_PRO_PARA;
	for (int i = 0; i < 16; i++)
	{
		set_netkey_message.netKey[i] = netKey[i];
	}
	set_netkey_message.magic = 0x44332211;
	set_netkey_message.addr = 0x01;
	// gateway->setBleAddr(0x01);
	// database->GatewayUpdateUnicast(gateway, 0x01);
	// gateway->setBleIvIndex(bswap_32(set_netkey_message.magic));
	// database->GatewayUpdateIvIndex(gateway, bswap_32(set_netkey_message.magic));
	return SendMessage(SYSTEM_REQ, (uint8_t *)&set_netkey_message, sizeof(set_netkey_message_t), HCI_GATEWAY_CMD_SEND_IVI, 0, 0, 1000);
}

int BleProtocol::SetGwKey()
{
	LOGD("SetGwKey");
	typedef struct __attribute__((packed))
	{
		uint8_t opcode;
		uint8_t addr[2];
		uint8_t gwKey[16];
	} set_gwkey_message_t;
	set_gwkey_message_t set_gwkey_message;
	set_gwkey_message.opcode = 0x0D;
	set_gwkey_message.addr[0] = 0x01;
	set_gwkey_message.addr[1] = 0x00;
	for (int i = 0; i < 16; i++)
	{
		set_gwkey_message.gwKey[i] = gwKey[i];
	}
	return SendMessage(SYSTEM_REQ, (uint8_t *)&set_gwkey_message, sizeof(set_gwkey_message_t), 0, 0, 0, 1000);
}

int BleProtocol::StartScan()
{
	LOGD("StartScan BLE");
	uint8_t d = HCI_GATEWAY_CMD_START;
	// SetProvisioning(true);
	int rs = SendMessage(SYSTEM_REQ, &d, 1, 0, 0, 0, 0);
	if (rs)
	{
		LOGE("Send start scan error, rs: %d", rs);
	}
	return rs;
}

int BleProtocol::StopScan()
{
	LOGD("StopScan");
	uint8_t d = HCI_GATEWAY_CMD_STOP;
	SetProvisioning(false);
	int rs = SendMessage(SYSTEM_REQ, &d, 1, 0, 0, 0, 0);
	if (rs)
	{
		LOGE("Send stop scan error, rs: %d", rs);
	}
	return rs;
}

int BleProtocol::ResetBle()
{
	LOGD("ResetBle");
	uint8_t d = HCI_GATEWAY_CMD_RESET;
	int rs = SendMessage(SYSTEM_REQ, &d, 1, 0, 0, 0, 8000);
	if (rs)
	{
		LOGE("Send reset factory error, rs: %d", rs);
	}
	return rs;
}

int BleProtocol::ResetFactory()
{
	LOGD("ResetFactory");
	InitKey();
	return CODE_OK;
}

static uint32_t convertDeviceType(uint32_t type)
{
	uint8_t *arr = (uint8_t *)&type;
	return (arr[0] + (arr[1] * 1000) + (arr[2] * 10000));
}

bool BleProtocol::IsProvision()
{
	return isProvisioning;
}

void BleProtocol::SetProvisioning(bool isProvision)
{
	this->isProvisioning = isProvision;
}

int BleProtocol::AddDevice(scan_device_message_t *scan_device_message)
{
	LOGD("AddDevice");
	uint16_t version = 0;
	uint32_t deviceType = 0;
	uuid_t *uuid = (uuid_t *)scan_device_message->uuid;
	string mac = Util::ConvertU32ToHexString(scan_device_message->mac, sizeof(scan_device_message->mac));
	LOGI("Scan device mac 0x%s, rssi: %i", mac.c_str(), scan_device_message->rssi);
	int rs = CODE_ERROR;
	if (IsProvision() && !SelectMac(scan_device_message->mac))
	{
		if (IsProvision() && !GetNetKey())
		{
			if (IsProvision() && !Provision(nextAddr))
			{
				if (IsProvision() && !BindingAll())
				{
					if (IsProvision() && !SetGwAddr(nextAddr))
					{
						if (IsProvision() && !GetDeviceType(scan_device_message->mac, nextAddr, deviceType, version))
						{
							LOGI("Add new device id: %s, name: %s, mac: %s, addr: 0x%04X, type: 0x%04X, verion: %d",
								 Util::uuidToStr(uuid->uuid).c_str(), "Cong tac cam ung", mac.c_str(), nextAddr, deviceType, version);
							rs = CODE_OK;
						}
					}
				}
				if (rs != CODE_OK)
				{
					ResetDev(nextAddr);
				}
			}
		}
	}

	if (IsProvision())
	{
		sleep(1);
		if (IsProvision())
			StartScan();
	}
	return rs;
}

int BleProtocol::SelectMac(uint8_t *mac)
{
	LOGD("SelectMac");
	uint8_t data[7];
	data[0] = HCI_GATEWAY_CMD_SET_ADV_FILTER;
	for (int i = 0; i < 6; i++)
	{
		data[i + 1] = mac[i];
	}
	return SendMessage(SYSTEM_REQ, data, sizeof(data), 0, 0, 0, 1000);
}

int BleProtocol::Provision(uint16_t deviceAddr)
{
	LOGD("Provision: deviceAddr 0x%x", deviceAddr);
	uint8_t dataRsp[100];
	int lenRsp;
	typedef struct __attribute__((packed))
	{
		uint8_t opcode;
		pro_net_info_t data_pro;
	} provision_message_t;
	provision_message_t provision_message;
	memset(&provision_message, 0x00, sizeof(provision_message));
	provision_message.opcode = HCI_GATEWAY_CMD_SET_NODE_PARA;
	memcpy(&provision_message.data_pro.netKey[0], &pro_net_info.netKey[0], sizeof(pro_net_info_t));
	provision_message.data_pro.unicast_address = deviceAddr;
	int rs = SendMessage(SYSTEM_REQ, (uint8_t *)&provision_message, sizeof(provision_message_t), HCI_GATEWAY_CMD_PROVISION_EVT, dataRsp, &lenRsp, 30000);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint8_t status;
			uint8_t data[24];
		} provision_rsp_message_t;
		provision_rsp_message_t *provision_rsp_message = (provision_rsp_message_t *)dataRsp;
		if (provision_rsp_message->status)
		{
			LOGD("Provision OK");
			return CODE_OK;
		}
		else
		{
			LOGW("Must hard reset Ble module");
		}
	}
	LOGW("Provision err");
	return CODE_ERROR;
}

int BleProtocol::BindingAll()
{
	LOGD("BindingAll");
	uint8_t dataRsp[100];
	int lenRsp;
	typedef struct __attribute__((packed))
	{
		uint8_t opcode;
		uint8_t rev[3];
		uint8_t appKey[16];
	} binding_all_message_t;
	binding_all_message_t binding_all_message;
	memset(&binding_all_message, 0x00, sizeof(binding_all_message));
	binding_all_message.opcode = HCI_GATEWAY_CMD_START_KEYBIND;
	for (int i = 0; i < 16; i++)
	{
		binding_all_message.appKey[i] = appKey[i];
	}
	int rs = SendMessage(SYSTEM_REQ, (uint8_t *)&binding_all_message, sizeof(binding_all_message_t), HCI_GATEWAY_CMD_KEY_BIND_EVT, dataRsp, &lenRsp, 30000);
	if (rs == CODE_OK)
	{
		if (lenRsp == 1 && dataRsp[0] == 1)
		{
			LOGD("BindingAll OK");
			return CODE_OK;
		}
	}
	LOGW("BindingAll err");
	return CODE_ERROR;
}

int BleProtocol::SetGwAddr(uint16_t devAddr, uint16_t gwAddrSet)
{
	LOGD("SetGwAddr");
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t setGwAddrHeader[] = {RD_OPCODE_PROVISION_RSP, 0x11, 0x02, 0x02, 0x00};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t gwAddr;
		uint8_t data[4];
	} set_gw_addr_message_t;
	set_gw_addr_message_t set_gw_addr_message;
	memset(&set_gw_addr_message, 0x00, sizeof(set_gw_addr_message));
	set_gw_addr_message.ble_message_header.devAddr = devAddr;
	set_gw_addr_message.opcodeVendor = RD_OPCODE_PROVISION;
	set_gw_addr_message.vendorId = RD_VENDOR_ID;
	set_gw_addr_message.opcodeRsp = RD_OPCODE_PROVISION_RSP;
	set_gw_addr_message.header = RD_OPCODE_PROVISION_SET_GW_ADDR;
	set_gw_addr_message.gwAddr = gwAddrSet;
	int rs = SendMessage(APP_REQ, (uint8_t *)&set_gw_addr_message, sizeof(set_gw_addr_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 5000, setGwAddrHeader, 4, 5);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcode[3];
			uint8_t header[2];
			uint8_t rev[6];
		} set_gw_addr_rsp_message_t;
		set_gw_addr_rsp_message_t *set_gw_addr_rsp_message = (set_gw_addr_rsp_message_t *)dataRsp;
		if (set_gw_addr_rsp_message->devAddr == devAddr)
		{
			if (set_gw_addr_rsp_message->opcode[0] == 0xE1 && set_gw_addr_rsp_message->opcode[1] == 0x11 && set_gw_addr_rsp_message->opcode[2] == 0x02 && set_gw_addr_rsp_message->header[0] == 0x02 && set_gw_addr_rsp_message->header[1] == 0x00)
			{
				LOGD("SetGwAddr OK");
				return CODE_OK;
			}
		}
	}
	LOGW("SetGwAddr err");
	return CODE_ERROR;
}

/**
 * @brief Gen Ble security key (6 bytes)
 *
 * @param mac mac of destination device
 * @param devAddr unicast addr of destination device
 * @param out out buffer to write key
 */
static void genSecurityKey(uint8_t *mac, uint16_t devAddr, uint8_t *out)
{
	AES aes(AESKeyLength::AES_128);
	memcpy(plaintext + 8, mac, 6);
	memcpy(plaintext + 14, (uint8_t *)&devAddr, 2);
	for (int n = 0; n < 16; n++)
	{
		printf("%02x ", plaintext[n]);
	}
	printf("\n");
	unsigned char *outAes = aes.EncryptECB(plaintext, 32, keyAes);
	for (int j = 0; j < 32; j++)
	{
		printf("%02x ", outAes[j]);
	}
	printf("\n");
	for (int i = 0; i < 6; i++)
	{
		out[i] = outAes[i + 10];
	}
	free(outAes);
}

int BleProtocol::GetDeviceType(uint8_t *mac, uint16_t devAddr, uint32_t &deviceType, uint16_t &deviceVersion)
{
	LOGD("GetDeviceType");
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t checkTypeHeader[] = {0xe1, 0x11, 0x02, 0x03, 0x00};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t data[6];
	} check_type_message_t;
	check_type_message_t check_type_message;
	memset(&check_type_message, 0x00, sizeof(check_type_message));
	check_type_message.ble_message_header.devAddr = devAddr;
	check_type_message.opcodeVendor = RD_OPCODE_PROVISION;
	check_type_message.vendorId = RD_VENDOR_ID;
	check_type_message.opcodeRsp = RD_OPCODE_PROVISION_RSP;
	check_type_message.header = RD_OPCODE_PROVISION_GET_DEV_TYPE;
	genSecurityKey(mac, devAddr, &check_type_message.data[0]);
	int rs = SendMessage(APP_REQ, (uint8_t *)&check_type_message, sizeof(check_type_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 5000, checkTypeHeader, 4, 5);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcode[3];
			uint8_t header[2];
			uint8_t deviceType[3];
			uint8_t magic;
			uint8_t version[2];
		} check_type_rsp_message_t;
		check_type_rsp_message_t *check_type_rsp_message = (check_type_rsp_message_t *)dataRsp;
		if (check_type_rsp_message->devAddr == devAddr)
		{
			if (check_type_rsp_message->opcode[0] == 0xE1 && check_type_rsp_message->opcode[1] == 0x11 && check_type_rsp_message->opcode[2] == 0x02 && check_type_rsp_message->header[0] == 0x03 && check_type_rsp_message->header[1] == 0x00)
			{
				deviceType = (check_type_rsp_message->deviceType[0] << 16) | (check_type_rsp_message->deviceType[1] << 8) | check_type_rsp_message->deviceType[2];
				deviceVersion = (check_type_rsp_message->version[0] << 8) | (check_type_rsp_message->version[1]);
				LOGD("GetDeviceType OK, deviceType: 0x%04X, version: %d", deviceType, deviceVersion);
				return CODE_OK;
			}
		}
	}
	LOGW("GetDeviceType err");
	return CODE_ERROR;
}

int BleProtocol::ResetDev(uint16_t devAddr)
{
	LOGD("Reset dev addr: 0x%04X", devAddr);
	uint8_t dataRsp[100];
	int lenRsp;
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint16_t opcode;
	} reset_message_t;
	reset_message_t reset_message = {0};
	memset(&reset_message, 0x00, sizeof(reset_message));
	uint8_t resetHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0x80, 0x4a};
	reset_message.ble_message_header.devAddr = devAddr;
	reset_message.opcode = NODE_RESET;
	int rs = SendMessage(APP_REQ, (uint8_t *)&reset_message, sizeof(reset_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, resetHeader, 0, 6);
	if (rs == CODE_OK)
	{
		return CODE_OK;
	}
	return CODE_ERROR;
}

int BleProtocol::ResetDelAll()
{
	LOGD("Reset all dev addr");
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t data[6];
	} reset_message_t;
	reset_message_t reset_message = {0};
	memset(&reset_message, 0x00, sizeof(reset_message));
	reset_message.ble_message_header.devAddr = 0xFFFF;
	reset_message.opcodeVendor = RD_OPCODE_PROVISION;
	reset_message.vendorId = RD_VENDOR_ID;
	reset_message.opcodeRsp = RD_OPCODE_PROVISION_RSP;
	reset_message.header = 0xFFFF;
	for (int i = 0; i < 6; i++)
	{
		reset_message.data[i] = i + 1;
	}

	int rs = SendMessage(APP_REQ, (uint8_t *)&reset_message, sizeof(reset_message_t), 0, 0, 0, 1000);
	if (rs == CODE_OK)
	{
		return CODE_OK;
	}
	return CODE_ERROR;
}

int BleProtocol::GetTTL(uint16_t devAddr)
{
	uint8_t dataRsp[100];
	int lenRsp;
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint16_t opcode;
	} ttl_message_t;
	ttl_message_t ttl_message = {0};
	memset(&ttl_message, 0x00, sizeof(ttl_message));
	uint8_t getOnOffHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0x82, 0x04};
	ttl_message.ble_message_header.devAddr = devAddr;
	ttl_message.opcode = CFG_DEFAULT_TTL_GET;
	int rs = SendMessage(APP_REQ, (uint8_t *)&ttl_message, sizeof(ttl_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 800, getOnOffHeader, 0, 6);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint16_t opcode;
			uint8_t data[1];
		} ttl_rsp_message_t;
		ttl_rsp_message_t *ttl_rsp_message = (ttl_rsp_message_t *)dataRsp;
		if (ttl_rsp_message->opcode == CFG_DEFAULT_TTL_STATUS)
		{
			return CODE_OK;
		}
	}
	return CODE_ERROR;
}

int BleProtocol::SendOnlineCheck(uint16_t devAddr, uint32_t typeDev, uint16_t version)
{
	return CODE_OK;
}

int BleProtocol::SetOnOffLight(uint16_t devAddr, uint8_t onoff, uint16_t transition, bool ack)
{
	LOGD("Set OnOff addr: 0x%04X value %d", devAddr, onoff);
	uint8_t dataRsp[100];
	int lenRsp;
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint16_t opcode;
		uint8_t onoff;
		uint8_t rev2;
		uint16_t transition;
	} onoff_message_t;
	onoff_message_t onoff_message = {0};
	memset(&onoff_message, 0x00, sizeof(onoff_message));
	if (ack)
	{
		struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint16_t opcodeRsp;
		} turnOnOffHeader = {
			.devAddr = devAddr,
			.gwAddr = 0x0001,
			.opcodeRsp = G_ONOFF_STATUS,
		};
		onoff_message.ble_message_header.devAddr = devAddr;
		onoff_message.opcode = G_ONOFF_SET;
		onoff_message.onoff = onoff;
		onoff_message.rev2 = 0;
		onoff_message.transition = transition;
		int rs = 0;
		rs = SendMessage(APP_REQ, (uint8_t *)&onoff_message, sizeof(onoff_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, (uint8_t *)&turnOnOffHeader, 0, sizeof(turnOnOffHeader));
		if (rs == CODE_OK)
		{
			typedef struct __attribute__((packed))
			{
				uint16_t devAddr;
				uint16_t gwAddr;
				uint16_t opcode;
				uint8_t data[3];
			} onoff_rsp_message_t;
			onoff_rsp_message_t *onoff_rsp_message = (onoff_rsp_message_t *)dataRsp;
			if (onoff_rsp_message->opcode == G_ONOFF_STATUS)
			{
				if (lenRsp == 7)
				{
					if (onoff == onoff_rsp_message->data[0])
						return CODE_OK;
				}
				else
				{
					if (onoff == onoff_rsp_message->data[1])
						return CODE_OK;
				}
				LOGW("Onoff resp state not match with input control");
			}
		}
	}
	else
	{
		onoff_message.ble_message_header.devAddr = devAddr;
		onoff_message.opcode = G_ONOFF_SET_NOACK;
		onoff_message.onoff = onoff;
		onoff_message.rev2 = 0;
		onoff_message.transition = transition;
		int rs = SendMessage(APP_REQ, (uint8_t *)&onoff_message, sizeof(onoff_message_t), 0, dataRsp, &lenRsp, 1000);
		if (rs == CODE_OK)
		{
			return CODE_OK;
		}
	}
	LOGW("SetOnOff err");
	return CODE_ERROR;
}

int BleProtocol::GetOnoffLight(uint16_t devAddr)
{
	LOGD("Get OnOff addr: 0x%04X", devAddr);
	uint8_t dataRsp[100];
	int lenRsp;
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint16_t opcode;
	} onoff_message_t;
	onoff_message_t onoff_message = {0};
	memset(&onoff_message, 0x00, sizeof(onoff_message));
	uint8_t getOnOffHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0x82, 0x04};
	onoff_message.ble_message_header.devAddr = devAddr;
	onoff_message.opcode = G_ONOFF_GET;
	int rs = SendMessage(APP_REQ, (uint8_t *)&onoff_message, sizeof(onoff_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 800, getOnOffHeader, 0, 6);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint16_t opcode;
			uint8_t data[3];
		} onoff_rsp_message_t;
		onoff_rsp_message_t *onoff_rsp_message = (onoff_rsp_message_t *)dataRsp;
		if (onoff_rsp_message->opcode == G_ONOFF_STATUS)
		{
			return CODE_OK;
		}
	}

	LOGW("GetOnOff err");
	return CODE_ERROR;
}

int BleProtocol::SetDimmingLight(uint16_t devAddr, uint16_t dim, uint16_t transition, bool ack)
{
	LOGD("Dimming addr: 0x%04X value %d", devAddr, dim);
	uint8_t dataRsp[100];
	int lenRsp;
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint16_t opcode;
		uint16_t dim;
		uint8_t offset;
		uint16_t transition;
	} dim_message_t;
	dim_message_t dim_message;
	memset(&dim_message, 0x00, sizeof(dim_message));
	if (ack)
	{
		uint8_t dimmingHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0x82, 0x4E};

		dim_message.ble_message_header.devAddr = devAddr;
		dim_message.opcode = LIGHTNESS_SET;
		dim_message.dim = dim;
		dim_message.offset = 0;
		dim_message.transition = transition;
		int rs = SendMessage(APP_REQ, (uint8_t *)&dim_message, sizeof(dim_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, dimmingHeader, 0, 6);
		if (rs == CODE_OK)
		{
			typedef struct __attribute__((packed))
			{
				uint16_t devAddr;
				uint16_t gwAddr;
				uint16_t opcode;
				uint8_t data[8];
			} dim_rsp_message_t;
			dim_rsp_message_t *dim_rsp_message = (dim_rsp_message_t *)dataRsp;
			if (lenRsp == 8)
			{
				if ((dim_rsp_message->data[0] | dim_rsp_message->data[1] << 8) == dim)
				{
					return CODE_OK;
				}
			}
			else if (lenRsp > 8)
			{
				if ((dim_rsp_message->data[2] | dim_rsp_message->data[3] << 8) == dim)
				{
					return CODE_OK;
				}
			}
		}
	}
	else
	{
		dim_message.ble_message_header.devAddr = devAddr;
		dim_message.opcode = LIGHTNESS_SET_NOACK;
		dim_message.dim = dim;
		dim_message.offset = 0;
		dim_message.transition = transition;
		int rs = SendMessage(APP_REQ, (uint8_t *)&dim_message, sizeof(dim_message_t), 0, dataRsp, &lenRsp, 1000);
		if (rs == CODE_OK)
		{
			return CODE_OK;
		}
	}
	LOGW("Dimming err");
	return CODE_ERROR;
}

int BleProtocol::SetCctLight(uint16_t devAddr, uint16_t cct, uint16_t transition, bool ack)
{
	LOGD("Set Cct addr: 0x%04X value %d", devAddr, cct);
	uint8_t dataRsp[100];
	int lenRsp;
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint16_t opcode;
		uint16_t cct;
		uint8_t offset[3];
		uint16_t transition;
	} cct_message_t;
	cct_message_t cct_message;
	memset(&cct_message, 0x00, sizeof(cct_message));
	if (ack)
	{
		uint8_t cctHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 00, 0x82, 0x66};
		cct_message.ble_message_header.devAddr = devAddr;
		cct_message.opcode = LIGHT_CTL_TEMP_SET;
		cct_message.cct = cct;
		for (int count = 0; count < 3; count++)
		{
			cct_message.offset[count] = 0;
		}
		cct_message.transition = transition;
		int rs = SendMessage(APP_REQ, (uint8_t *)&cct_message, sizeof(cct_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, cctHeader, 0, 6);
		if (rs == CODE_OK)
		{
			typedef struct __attribute__((packed))
			{
				uint16_t devAddr;
				uint16_t gwAddr;
				uint16_t opcode;
				uint8_t data[8];
			} cct_rsp_message_t;
			cct_rsp_message_t *cct_rsp_message = (cct_rsp_message_t *)dataRsp;
			if (lenRsp == 10)
			{
				if (cct == (cct_rsp_message->data[0] | (cct_rsp_message->data[1] << 8)))
				{
					return CODE_OK;
				}
			}
			else if (lenRsp > 10)
			{
				if (cct == (cct_rsp_message->data[4] | (cct_rsp_message->data[5] << 8)))
				{
					return CODE_OK;
				}
			}
		}
	}
	else if (ack == false)
	{
		cct_message.ble_message_header.devAddr = devAddr;
		cct_message.opcode = LIGHT_CTL_TEMP_SET_NOACK;
		cct_message.cct = cct;
		for (int count = 0; count < 3; count++)
		{
			cct_message.offset[count] = 0;
		}
		cct_message.transition = transition;
		int rs = SendMessage(APP_REQ, (uint8_t *)&cct_message, sizeof(cct_message_t), 0, dataRsp, &lenRsp, 1000);
		if (rs == CODE_OK)
		{
			return CODE_OK;
		}
	}
	LOGW("Cct err");
	return CODE_ERROR;
}

int BleProtocol::SetHSLLight(uint16_t devAddr, uint16_t H, uint16_t S, uint16_t L, uint16_t transition, bool ack)
{
	LOGD("HSL addr: 0x%04X value HSL: %d-%d-%d", devAddr, H, S, L);
	uint8_t dataRsp[100];
	int lenRsp;
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint16_t opcode;
		uint16_t l;
		uint16_t h;
		uint16_t s;
		uint8_t offset;
		uint16_t transition;
	} hsl_message_t;
	hsl_message_t hsl_message = {0};
	memset(&hsl_message, 0x00, sizeof(hsl_message));
	if (ack)
	{
		uint8_t hslHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0x82, 0x78};
		hsl_message.ble_message_header.devAddr = devAddr;
		hsl_message.opcode = LIGHT_HSL_SET;
		hsl_message.l = L;
		hsl_message.h = H;
		hsl_message.s = S;
		hsl_message.offset = 0;
		hsl_message.transition = transition;
		int rs = SendMessage(APP_REQ, (uint8_t *)&hsl_message, sizeof(hsl_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, hslHeader, 0, 6);
		if (rs == CODE_OK)
		{
			typedef struct __attribute__((packed))
			{
				uint16_t devAddr;
				uint16_t gwAddr;
				uint16_t opcode;
				uint16_t l;
				uint16_t h;
				uint16_t s;
			} hsl_rsp_message_t;
			hsl_rsp_message_t *hsl_rsp_message = (hsl_rsp_message_t *)dataRsp;
			if (hsl_rsp_message->h == H && hsl_rsp_message->l == L && hsl_rsp_message->s == S)
			{
				return CODE_OK;
			}
			LOGW("hsl resp state not match with input control");
		}
	}
	else
	{
		hsl_message.ble_message_header.devAddr = devAddr;
		hsl_message.opcode = LIGHT_HSL_SET_NOACK;
		hsl_message.l = L;
		hsl_message.h = H;
		hsl_message.s = S;
		hsl_message.offset = 0;
		hsl_message.transition = transition;
		int rs = SendMessage(APP_REQ, (uint8_t *)&hsl_message, sizeof(hsl_message_t), 0, dataRsp, &lenRsp, 1000);
		if (rs == CODE_OK)
		{
			return CODE_OK;
		}
	}
	LOGW("Set hsl err");
	return CODE_ERROR;
}

int BleProtocol::SetCctDimLight(uint16_t devAddr, uint16_t cct, uint16_t dim, uint16_t transition, bool ack)
{
	LOGD("Set Dim cct addr: 0x%04X", devAddr);
	uint8_t dataRsp[100];
	int lenRsp;
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint16_t opcode;
		uint16_t dim;
		uint16_t cct;
		uint8_t offset;
		uint16_t transition;
	} dimcct_message_t;
	dimcct_message_t dimcct_message = {0};
	memset(&dimcct_message, 0x00, sizeof(dimcct_message));
	if (ack)
	{
		uint8_t dimcctHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0x82, 0x60};
		dimcct_message.ble_message_header.devAddr = devAddr;
		dimcct_message.opcode = LIGHT_CTL_SET;
		dimcct_message.dim = dim;
		dimcct_message.cct = cct;
		dimcct_message.offset = 0;
		dimcct_message.transition = transition;
		int rs = SendMessage(APP_REQ, (uint8_t *)&dimcct_message, sizeof(dimcct_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, dimcctHeader, 0, 6);
		if (rs == CODE_OK)
		{
			typedef struct __attribute__((packed))
			{
				uint16_t devAddr;
				uint16_t gwAddr;
				uint16_t opcode;
				uint16_t dim;
				uint16_t cct;
			} dimcct_rsp_message_t;
			dimcct_rsp_message_t *dimcct_rsp_message = (dimcct_rsp_message_t *)dataRsp;
			if (dimcct_rsp_message->dim == dim && dimcct_rsp_message->cct == cct)
			{
				return CODE_OK;
			}
			LOGW("dim cct resp state not match with input control");
		}
	}
	else
	{
		dimcct_message.ble_message_header.devAddr = devAddr;
		dimcct_message.opcode = LIGHT_CTL_SET_NOACK;
		dimcct_message.dim = dim;
		dimcct_message.cct = cct;
		dimcct_message.offset = 0;
		dimcct_message.transition = transition;
		int rs = SendMessage(APP_REQ, (uint8_t *)&dimcct_message, sizeof(dimcct_message_t), 0, dataRsp, &lenRsp, 1000);
		if (rs == CODE_OK)
		{
			return CODE_OK;
		}
	}
	LOGW("Set dim cct err");
	return CODE_ERROR;
}

int BleProtocol::AddDev2Group(uint16_t devAddr, uint16_t element, uint16_t group)
{
	LOGD("Add dev addr: 0x%04X  with element: 0x%04x to group: 0x%04X", devAddr, element, group);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t addGroupHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0x80, 0x1f};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint16_t opcode;
		uint16_t element;
		uint16_t group;
		uint16_t offset;
	} addgroup_message_t;
	addgroup_message_t addgroup_message = {0};
	memset(&addgroup_message, 0x00, sizeof(addgroup_message));
	addgroup_message.ble_message_header.devAddr = devAddr;
	addgroup_message.opcode = CFG_MODEL_SUB_ADD;
	addgroup_message.element = element;
	addgroup_message.group = group;
	addgroup_message.offset = 0x1000;
	int rs = SendMessage(APP_REQ, (uint8_t *)&addgroup_message, sizeof(addgroup_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 2000, addGroupHeader, 0, 6);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint16_t opcode;
			uint8_t offset;
			uint16_t element;
			uint16_t group;
		} addgroup_rsp_message_t;
		addgroup_rsp_message_t *addgroup_rsp_message = (addgroup_rsp_message_t *)dataRsp;
		if (element == addgroup_rsp_message->element && group == addgroup_rsp_message->group)
		{
			return CODE_OK;
		}
		LOGW("add group resp state not match with input control");
	}
	LOGW("Add group err");
	return CODE_ERROR;
}

int BleProtocol::DelDev2Group(uint16_t devAddr, uint16_t element, uint16_t group)
{
	LOGD("Del dev addr: 0x%04X  with element: 0x%04x to group: 0x%04X", devAddr, element, group);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t delGroupHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0x80, 0x1f};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint16_t opcode;
		uint16_t element;
		uint16_t group;
		uint16_t offset;
	} delgroup_message_t;
	delgroup_message_t delgroup_message = {0};
	memset(&delgroup_message, 0x00, sizeof(delgroup_message));
	delgroup_message.ble_message_header.devAddr = devAddr;
	delgroup_message.opcode = CFG_MODEL_SUB_DEL;
	delgroup_message.element = element;
	delgroup_message.group = group;
	delgroup_message.offset = 0x1000;
	int rs = SendMessage(APP_REQ, (uint8_t *)&delgroup_message, sizeof(delgroup_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 2000, delGroupHeader, 0, 6);
	if (rs == CODE_OK)
	{
		return CODE_OK;
	}
	LOGW("Del group err");
	return CODE_ERROR;
}

int BleProtocol::AddDev2Room(uint16_t devAddr, uint16_t group, uint16_t scene)
{
	LOGW("Add dev addr: 0x%04X to room: 0x%04X", devAddr, group);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t addDev2RoomHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t groupId;
		uint16_t sceneId;
		uint8_t data[2];
	} addDev2Room_t;
	addDev2Room_t addDev2Room = {0};
	memset(&addDev2Room, 0x00, sizeof(addDev2Room));
	addDev2Room.ble_message_header.devAddr = devAddr;
	addDev2Room.opcodeVendor = RD_OPCODE_CONFIG;
	addDev2Room.vendorId = RD_VENDOR_ID;
	addDev2Room.opcodeRsp = RD_OPCODE_PROVISION_RSP;
	addDev2Room.header = RD_OPCODE_CONFIG_ADD_ROOM;
	addDev2Room.groupId = group;
	addDev2Room.sceneId = scene;
	int rs = SendMessage(APP_REQ, (uint8_t *)&addDev2Room, sizeof(addDev2Room_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, addDev2RoomHeader, 0, 7);
	if (rs != CODE_OK)
		LOGW("AddDev2Room error");
	return rs;
}

int BleProtocol::DelDev2Room(uint16_t devAddr, uint16_t room)
{
	LOGW("Del dev addr: 0x%04X from room: 0x%04X", devAddr, room);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t delDev2RoomHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t data[6];
	} delDev2Room_t;
	delDev2Room_t delDev2Room = {0};
	memset(&delDev2Room, 0x00, sizeof(delDev2Room));
	delDev2Room.ble_message_header.devAddr = devAddr;
	delDev2Room.opcodeVendor = RD_OPCODE_CONFIG;
	delDev2Room.vendorId = RD_VENDOR_ID;
	delDev2Room.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	delDev2Room.header = RD_OPCODE_CONFIG_DEL_ROOM;

	int rs = SendMessage(APP_REQ, (uint8_t *)&delDev2Room, sizeof(delDev2Room_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, delDev2RoomHeader, 0, 7);
	if (rs != CODE_OK)
		LOGW("DelDev2Room error");
	return rs;
}

int BleProtocol::SetSceneBle(uint16_t devAddr, uint16_t scene, uint8_t modeRgb)
{
	LOGD("Set scene addr: 0x%04X to scene: 0x%04X, modergb: %d", devAddr, scene, modeRgb);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t setSceneHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0x82, 0x45};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint16_t opcode;
		uint16_t scene;
		uint8_t modeRgb;
		uint16_t offset;
	} setscene_message_t;
	setscene_message_t setscene_message = {0};
	memset(&setscene_message, 0x00, sizeof(setscene_message));
	setscene_message.ble_message_header.devAddr = devAddr;
	setscene_message.opcode = SCENE_STORE;
	setscene_message.scene = scene;
	setscene_message.modeRgb = modeRgb;
	int rs = SendMessage(APP_REQ, (uint8_t *)&setscene_message, sizeof(setscene_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, setSceneHeader, 0, 6);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint16_t opcode;
			uint8_t offset;
			uint16_t scene;
		} setscene_rsp_message_t;
		setscene_rsp_message_t *setscene_rsp_message = (setscene_rsp_message_t *)dataRsp;
		if (setscene_rsp_message->scene == scene)
		{
			return CODE_OK;
		}
		else
		{
			LOGW("set scene resp state not match with input control");
		}
	}
	LOGW("Set scene err");
	return CODE_ERROR;
}

// TODO: BelProtocol DelScene
int BleProtocol::DelSceneBle(uint16_t devAddr, uint16_t scene)
{
	LOGD("Del scene addr: 0x%04X to scene: 0x%04X", devAddr, scene);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t delSceneHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0x82, 0x45};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint16_t opcode;
		uint16_t scene;
	} delscene_message_t;
	delscene_message_t delscene_message = {0};
	memset(&delscene_message, 0x00, sizeof(delscene_message));
	delscene_message.ble_message_header.devAddr = devAddr;
	delscene_message.opcode = SCENE_DEL;
	delscene_message.scene = scene;
	int rs = SendMessage(APP_REQ, (uint8_t *)&delscene_message, sizeof(delscene_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, delSceneHeader, 0, 6);
	if (rs == CODE_OK)
	{
		return CODE_OK;
	}
	LOGW("Del scene err");
	return CODE_ERROR;
}

// TODO: BelProtocol ActiveScene
// add delay time
int BleProtocol::CallScene(uint16_t devAddr, uint16_t scene, uint16_t transition, bool ack)
{
	LOGD("Call scene: 0x%04X", scene);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t callSceneHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0x5e, 0x00};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint16_t opcode;
		uint16_t scene;
		uint8_t offset;
		uint16_t transition;
	} callscene_message_t;
	callscene_message_t callscene_message = {0};
	memset(&callscene_message, 0x00, sizeof(callscene_message));
	if (ack)
	{
		callscene_message.ble_message_header.devAddr = devAddr;
		callscene_message.opcode = SCENE_RECALL;
		callscene_message.scene = scene;
		callscene_message.offset = 0;
		callscene_message.transition = transition;
		int rs = SendMessage(APP_REQ, (uint8_t *)&callscene_message, sizeof(callscene_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, callSceneHeader, 0, 6);
		if (rs == CODE_OK)
		{
			typedef struct __attribute__((packed))
			{
				uint16_t devAddr;
				uint16_t gwAddr;
				uint16_t opcode;
				uint8_t data[8];
			} callscene_rsp_message_t;
			callscene_rsp_message_t *callscene_rsp_message = (callscene_rsp_message_t *)dataRsp;
			if (lenRsp == 11 || lenRsp == 13)
			{
				if (scene == (callscene_rsp_message->data[2] | (callscene_rsp_message->data[3] << 8)))
				{
					return CODE_OK;
				}
				else
				{
					LOGW("call scene resp state not match with input control");
				}
			}
			else
			{
				if (scene == (callscene_rsp_message->data[0] | (callscene_rsp_message->data[1] << 8)))
				{
					return CODE_OK;
				}
				else
				{
					LOGW("call scene resp state not match with input control");
				}
			}
		}
	}
	else
	{
		callscene_message.ble_message_header.devAddr = devAddr;
		callscene_message.opcode = SCENE_RECALL_NOACK;
		callscene_message.scene = scene;
		callscene_message.offset = 0;
		callscene_message.transition = transition;
		int rs = SendMessage(APP_REQ, (uint8_t *)&callscene_message, sizeof(callscene_message_t), 0, dataRsp, &lenRsp, 1000);
		if (rs == CODE_OK)
		{
			return CODE_OK;
		}
	}

	LOGW("Call scene err");
	return CODE_ERROR;
}

int BleProtocol::CallModeRgb(uint16_t devAddr, uint8_t modeRgb)
{
	LOGD("Call modeRgb: %d, addr: 0x%04X ", modeRgb, devAddr);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t modeRgbHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0x82, 0x52};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint16_t opcode;
		uint16_t header;
		uint8_t mode;
	} modergb_message_t;
	modergb_message_t modergb_message = {0};
	memset(&modergb_message, 0x00, sizeof(modergb_message));
	modergb_message.ble_message_header.devAddr = devAddr;
	modergb_message.opcode = LIGHTNESS_LINEAR_SET;
	modergb_message.header = HEADER_CALLMODE_RGB;
	modergb_message.mode = modeRgb;
	int rs = SendMessage(APP_REQ, (uint8_t *)&modergb_message, sizeof(modergb_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, modeRgbHeader, 0, 6);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint16_t opcode;
			uint16_t header;
			uint8_t mode;
		} modergb_rsp_message_t;
		modergb_rsp_message_t *modergb_rsp_message = (modergb_rsp_message_t *)dataRsp;
		if (modergb_rsp_message->header == HEADER_CALLMODE_RGB)
		{
			if (modergb_rsp_message->mode == modeRgb)
			{
				return CODE_OK;
			}
			LOGW("call mode rgb resp state not match with input control");
		}
	}
	LOGW("call mode rgb err");
	return CODE_ERROR;
}

int BleProtocol::UpdateLights(uint16_t devAddr)
{
	LOGI("Update lights addr: 0x%04X ", devAddr);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t updateHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0x82, 0x52};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint16_t opcode;
		uint8_t header;
		uint8_t data[7];
	} update_message_t;
	update_message_t update_message = {0};
	memset(&update_message, 0x00, sizeof(update_message));
	update_message.ble_message_header.devAddr = devAddr;
	update_message.opcode = LIGHTNESS_LINEAR_SET;
	update_message.header = 0x02;
	int rs = SendMessage(APP_REQ, (uint8_t *)&update_message, sizeof(update_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 800, updateHeader, 0, 6);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint16_t opcode;
			uint8_t header;
		} update_rsp_message_t;
		update_rsp_message_t *update_rsp_message = (update_rsp_message_t *)dataRsp;
		if (update_rsp_message->header == 0x02)
		{
			return CODE_OK;
		}
		LOGW("update lights resp state not match with input control");
	}
	LOGW("update lights err");
	return CODE_ERROR;
}

int BleProtocol::UpdateStatusSensorsPm(uint16_t devAddr)
{
	LOGD("Update status sensor addr: 0x%04X ", devAddr);
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t data[6];
	} update_message_t;
	update_message_t update_message = {0};
	memset(&update_message, 0x00, sizeof(update_message));
	update_message.ble_message_header.devAddr = devAddr;
	update_message.opcodeVendor = RD_OPCODE_CONFIG;
	update_message.vendorId = RD_VENDOR_ID;
	update_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	update_message.header = GET_STATUS_SENSOR_PM;
	int rs = SendMessage(APP_REQ, (uint8_t *)&update_message, sizeof(update_message_t), HCI_GATEWAY_RSP_OP_CODE, 0, 0, 1000);
	if (rs == CODE_OK)
	{
		return CODE_OK;
	}
	LOGW("update status sensor err");
	return CODE_ERROR;
}

int BleProtocol::SetSceneSwitchSceneDC(uint16_t devAddr, uint8_t button, uint8_t mode, uint16_t sceneId, uint8_t type)
{
	LOGD("SetSceneSwitchSceneDC 0x%04x, button %d, mode %d, sceneId %d, type %d", devAddr, button, mode, sceneId, type);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t setSceneDcHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t button;
		uint8_t mode;
		uint16_t sceneId;
		uint8_t type;
		uint8_t future;
	} set_scene_dc_message_t;
	set_scene_dc_message_t set_scene_dc_message = {0};
	memset(&set_scene_dc_message, 0x00, sizeof(set_scene_dc_message));
	set_scene_dc_message.ble_message_header.devAddr = devAddr;
	set_scene_dc_message.opcodeVendor = RD_OPCODE_CONFIG;
	set_scene_dc_message.vendorId = RD_VENDOR_ID;
	set_scene_dc_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	set_scene_dc_message.header = RD_OPCODE_CONFIG_SET_SCENE_SWITCH_SCENE_DC;
	set_scene_dc_message.button = button;
	set_scene_dc_message.mode = mode;
	set_scene_dc_message.sceneId = sceneId;
	set_scene_dc_message.type = type;
	int rs = SendMessage(APP_REQ, (uint8_t *)&set_scene_dc_message, sizeof(set_scene_dc_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, setSceneDcHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint8_t button;
			uint8_t mode;
			uint16_t sceneId;
			uint8_t type;
		} set_scene_dc_rsp_message_t;
		set_scene_dc_rsp_message_t *set_scene_dc_rsp_message = (set_scene_dc_rsp_message_t *)dataRsp;
		if (set_scene_dc_rsp_message->header == 0x0102 && set_scene_dc_rsp_message->button == button && set_scene_dc_rsp_message->mode == mode && set_scene_dc_rsp_message->sceneId == sceneId && set_scene_dc_rsp_message->type == type)
		{
			return CODE_OK;
		}
		LOGW("set scene dc resp state not match with input control");
	}
	LOGW("set scene dc remote err");
	return CODE_ERROR;
}

int BleProtocol::SetSceneSwitchSceneAC(uint16_t devAddr, uint8_t button, uint8_t mode, uint16_t sceneId, uint8_t type)
{
	LOGD("SetSceneSwitchSceneAC 0x%04x, button %d, mode %d, sceneId %d, type %d", devAddr, button, mode, sceneId, type);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t setSceneAcHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t button;
		uint8_t mode;
		uint16_t sceneId;
		uint8_t type;
		uint8_t future;
	} set_scene_ac_message_t;
	set_scene_ac_message_t set_scene_ac_message = {0};
	memset(&set_scene_ac_message, 0x00, sizeof(set_scene_ac_message));
	set_scene_ac_message.ble_message_header.devAddr = devAddr;
	set_scene_ac_message.opcodeVendor = RD_OPCODE_CONFIG;
	set_scene_ac_message.vendorId = RD_VENDOR_ID;
	set_scene_ac_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	set_scene_ac_message.header = RD_OPCODE_CONFIG_SET_SCENE_SWITCH_SCENE_AC;
	set_scene_ac_message.button = button;
	set_scene_ac_message.mode = mode;
	set_scene_ac_message.sceneId = sceneId;
	set_scene_ac_message.type = type;
	int rs = SendMessage(APP_REQ, (uint8_t *)&set_scene_ac_message, sizeof(set_scene_ac_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, setSceneAcHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint8_t button;
			uint8_t mode;
			uint16_t sceneId;
			uint8_t type;
		} set_scene_ac_rsp_message_t;
		set_scene_ac_rsp_message_t *set_scene_ac_rsp_message = (set_scene_ac_rsp_message_t *)dataRsp;
		LOGD("Header: %d, Scene: %d", set_scene_ac_rsp_message->header, sceneId);
		if (set_scene_ac_rsp_message->header == 0x0103 && set_scene_ac_rsp_message->button == button && set_scene_ac_rsp_message->mode == mode && set_scene_ac_rsp_message->sceneId == sceneId && set_scene_ac_rsp_message->type == type)
		{
			return CODE_OK;
		}
		LOGW("set scene ac scene resp state not match with input control");
	}
	LOGW("set scene ac remote err");
	return CODE_ERROR;
}

int BleProtocol::DelSceneSwitchSceneDC(uint16_t devAddr, uint8_t button, uint8_t mode)
{
	LOGD("DelSceneSwitchSceneDC 0x%04x, button %d, mode %d", devAddr, button, mode);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t delSceneDcHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t button;
		uint8_t mode;
		uint8_t future[4];
	} del_scene_dc_message_t;
	del_scene_dc_message_t del_scene_dc_message = {0};
	memset(&del_scene_dc_message, 0x00, sizeof(del_scene_dc_message));
	del_scene_dc_message.ble_message_header.devAddr = devAddr;
	del_scene_dc_message.opcodeVendor = RD_OPCODE_CONFIG;
	del_scene_dc_message.vendorId = RD_VENDOR_ID;
	del_scene_dc_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	del_scene_dc_message.header = RD_OPCODE_CONFIG_DEL_SCENE_SWITCH_SCENE_DC;
	del_scene_dc_message.button = button;
	del_scene_dc_message.mode = mode;
	int rs = SendMessage(APP_REQ, (uint8_t *)&del_scene_dc_message, sizeof(del_scene_dc_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, delSceneDcHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint8_t button;
			uint8_t mode;
		} del_scene_dc_rsp_message_t;
		del_scene_dc_rsp_message_t *del_scene_dc_rsp_message = (del_scene_dc_rsp_message_t *)dataRsp;
		LOGD("Header: %d", del_scene_dc_rsp_message->header);
		if (del_scene_dc_rsp_message->header == 0x0202 && del_scene_dc_rsp_message->button == button && del_scene_dc_rsp_message->mode == mode)
		{
			return CODE_OK;
		}
		LOGW("del scene dc scene resp state not match with input control");
	}
	LOGW("del scene dc scene err");
	return CODE_ERROR;
}

int BleProtocol::DelSceneSwitchSceneAC(uint16_t devAddr, uint8_t button, uint8_t mode)
{
	LOGD("DelSceneSwitchSceneAC 0x%04x, button %d, mode %d", devAddr, button, mode);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t delSceneAcHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t button;
		uint8_t mode;
		uint8_t future[4];
	} del_scene_ac_message_t;
	del_scene_ac_message_t del_scene_ac_message = {0};
	memset(&del_scene_ac_message, 0x00, sizeof(del_scene_ac_message));
	del_scene_ac_message.ble_message_header.devAddr = devAddr;
	del_scene_ac_message.opcodeVendor = RD_OPCODE_CONFIG;
	del_scene_ac_message.vendorId = RD_VENDOR_ID;
	del_scene_ac_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	del_scene_ac_message.header = RD_OPCODE_CONFIG_DEL_SCENE_SWITCH_SCENE_AC;
	del_scene_ac_message.button = button;
	del_scene_ac_message.mode = mode;
	int rs = SendMessage(APP_REQ, (uint8_t *)&del_scene_ac_message, sizeof(del_scene_ac_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, delSceneAcHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint8_t button;
			uint8_t mode;
		} del_scene_ac_rsp_message_t;
		del_scene_ac_rsp_message_t *del_scene_ac_rsp_message = (del_scene_ac_rsp_message_t *)dataRsp;
		LOGD("Header: %d", del_scene_ac_rsp_message->header);
		if (del_scene_ac_rsp_message->header == 0x0203 && del_scene_ac_rsp_message->button == button && del_scene_ac_rsp_message->mode == mode)
		{
			return CODE_OK;
		}
		LOGW("del scene ac scene resp state not match with input control");
	}
	LOGW("del scene ac scene err");
	return CODE_ERROR;
}

int BleProtocol::SetScenePirLightSensor(uint16_t devAddr, uint8_t condition, uint8_t pir, uint16_t lowLux, uint16_t highLux, uint16_t scene, uint8_t type)
{
	LOGD("SetScenePirLightSensor condition: %d, pir: %d, lowLux: %d, highLux: %d", condition, pir, lowLux, highLux);
	typedef struct __attribute__((packed))
	{
		union
		{
			uint32_t data;
			struct
			{
				uint32_t store : 8;			 // 8 bit not use
				uint32_t Lux_hi : 10;		 // 10 bit lux hi
				uint32_t Lux_low : 10;		 // 10 bit lux low
				uint32_t Light_Conditon : 3; // 7 bit low
				uint32_t Pir_Conditon : 1;	 // 1 bit hight
			};
		};
	} RD_Sensor_data_tdef;

	RD_Sensor_data_tdef data_scene_pir_light;
	data_scene_pir_light.Pir_Conditon = (uint32_t)pir;
	data_scene_pir_light.Light_Conditon = (uint32_t)(condition & 0x0000007);
	data_scene_pir_light.Lux_low = (uint32_t)(lowLux);
	data_scene_pir_light.Lux_hi = (uint32_t)(highLux);

	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t sceneLightPirHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t sceneId;
		uint8_t infoScene[3];
		uint8_t type;
	} scene_light_pir_message_t;
	scene_light_pir_message_t scene_light_pir_message = {0};
	memset(&scene_light_pir_message, 0x00, sizeof(scene_light_pir_message));
	scene_light_pir_message.ble_message_header.devAddr = devAddr;
	scene_light_pir_message.opcodeVendor = RD_OPCODE_CONFIG;
	scene_light_pir_message.vendorId = RD_VENDOR_ID;
	scene_light_pir_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	scene_light_pir_message.header = RD_OPCODE_CONFIG_SET_SCENE_PIR_LIGHT_SENSOR;
	scene_light_pir_message.sceneId = scene;
	scene_light_pir_message.infoScene[0] = (data_scene_pir_light.data >> 24) & 0xFF;
	scene_light_pir_message.infoScene[1] = (data_scene_pir_light.data >> 16) & 0xFF;
	scene_light_pir_message.infoScene[2] = (data_scene_pir_light.data >> 8) & 0xFF;
	int rs = SendMessage(APP_REQ, (uint8_t *)&scene_light_pir_message, sizeof(scene_light_pir_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 2000, sceneLightPirHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint16_t scene;
		} scene_light_pir_rsp_message_t;
		scene_light_pir_rsp_message_t *scene_light_pir_rsp_message = (scene_light_pir_rsp_message_t *)&dataRsp;
		if (scene_light_pir_rsp_message->header == 0x0145 && scene_light_pir_rsp_message->scene == scene)
		{
			return CODE_OK;
		}
		else
		{
			LOGW("Scene light pir rsp not match control");
		}
	}
	else
	{
		LOGW("Scene light pir error");
	}
	return CODE_ERROR;
}

int BleProtocol::DelScenePirLightSensor(uint16_t devAddr, uint16_t scene)
{
	LOGD("Del scene light pir");
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t sceneLightPirHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t sceneId;
	} scene_light_pir_message_t;
	scene_light_pir_message_t scene_light_pir_message = {0};
	memset(&scene_light_pir_message, 0x00, sizeof(scene_light_pir_message));
	scene_light_pir_message.ble_message_header.devAddr = devAddr;
	scene_light_pir_message.opcodeVendor = RD_OPCODE_CONFIG;
	scene_light_pir_message.vendorId = RD_VENDOR_ID;
	scene_light_pir_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	scene_light_pir_message.header = RD_OPCODE_CONFIG_DEL_SCENE_PIR_LIGHT_SENSOR;
	scene_light_pir_message.sceneId = scene;
	int rs = SendMessage(APP_REQ, (uint8_t *)&scene_light_pir_message, sizeof(scene_light_pir_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 2000, sceneLightPirHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint16_t scene;
		} scene_light_pir_rsp_message_t;
		scene_light_pir_rsp_message_t *scene_light_pir_rsp_message = (scene_light_pir_rsp_message_t *)&dataRsp;
		if (scene_light_pir_rsp_message->header == 0x0245 && scene_light_pir_rsp_message->scene == scene)
		{
			return CODE_OK;
		}
		else
		{
			LOGW("Del Scene light pir rsp not match control");
		}
	}
	else
	{
		LOGW("Del Scene light pir error");
	}
	return CODE_ERROR;
}

int BleProtocol::TimeActionPirLightSensor(uint16_t devAddr, uint16_t time)
{
	LOGD("TimeActionPirLightSensor 0x%04X", devAddr);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t timeActionHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t time;
	} time_action_message_t;
	time_action_message_t time_action_message = {0};
	memset(&time_action_message, 0x00, sizeof(time_action_message));
	time_action_message.ble_message_header.devAddr = devAddr;
	time_action_message.opcodeVendor = RD_OPCODE_CONFIG;
	time_action_message.vendorId = RD_VENDOR_ID;
	time_action_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	time_action_message.header = RD_OPCODE_CONFIG_SET_TIME_ACTION_PIR_LIGHT_SENSOR;
	time_action_message.time = time;
	int rs = SendMessage(APP_REQ, (uint8_t *)&time_action_message, sizeof(time_action_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, timeActionHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint16_t time;
		} time_action_rsp_message_t;
		time_action_rsp_message_t *time_action_rsp_message = (time_action_rsp_message_t *)dataRsp;
		if (time_action_rsp_message->header == 0x0345 && time_action_rsp_message->time == time)
		{
			return CODE_OK;
		}
		LOGW("time action pir light resp state not match with input control");
	}
	LOGW("time action pir light err");
	return CODE_ERROR;
}

int BleProtocol::SetModeActionPirLightSensor(uint16_t devAddr, uint8_t mode)
{
	LOGD("ModeActionPirLightSensor 0x%04X", devAddr);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t modeActionHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t mode;
		uint8_t future[5];
	} mode_action_message_t;
	mode_action_message_t mode_action_message = {0};
	memset(&mode_action_message, 0x00, sizeof(mode_action_message));
	mode_action_message.ble_message_header.devAddr = devAddr;
	mode_action_message.opcodeVendor = RD_OPCODE_CONFIG;
	mode_action_message.vendorId = RD_VENDOR_ID;
	mode_action_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	mode_action_message.header = RD_OPCODE_CONFIG_SET_MODE_ACTION_PIR_LIGHT_SENSOR;
	mode_action_message.mode = mode;
	int rs = SendMessage(APP_REQ, (uint8_t *)&mode_action_message, sizeof(mode_action_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, modeActionHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint8_t mode;
		} mode_action_rsp_message_t;
		mode_action_rsp_message_t *mode_action_rsp_message = (mode_action_rsp_message_t *)dataRsp;
		if (mode_action_rsp_message->header == 0x0445 && mode_action_rsp_message->mode == mode)
		{
			return CODE_OK;
		}
		LOGW("mode action pir light resp state not match with input control");
	}
	LOGW("mode action pir light err");
	return CODE_ERROR;
}

int BleProtocol::SetSensiPirLightSensor(uint16_t devAddr, uint8_t sensi)
{
	LOGD("Set sensiPirLightSensor: 0x%04X, sensi: %d", devAddr, sensi);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t sensiHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t sensi;
	} sensi_message_t;
	sensi_message_t sensi_message = {0};
	memset(&sensi_message, 0x00, sizeof(sensi_message));
	sensi_message.ble_message_header.devAddr = devAddr;
	sensi_message.opcodeVendor = RD_OPCODE_CONFIG;
	sensi_message.vendorId = RD_VENDOR_ID;
	sensi_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	sensi_message.header = RD_OPCODE_CONFIG_SET_SENSI_PIR_LIGHT_SENSOR;
	sensi_message.sensi = sensi;
	int rs = SendMessage(APP_REQ, (uint8_t *)&sensi_message, sizeof(sensi_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, sensiHeader, 0, 7);
	if (rs == CODE_OK)
	{
		return CODE_OK;
	}
	else
		LOGW("Set sensi error");
	return CODE_ERROR;
}

int BleProtocol::SetDistanceSensor(uint16_t devAddr, uint8_t distance)
{
	LOGD("Set distance sensor: 0x%04X, distance: %d", devAddr, distance);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t distanceHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t distance;
	} distance_message_t;
	distance_message_t distance_message = {0};
	memset(&distance_message, 0x00, sizeof(distance_message));
	distance_message.ble_message_header.devAddr = devAddr;
	distance_message.opcodeVendor = RD_OPCODE_CONFIG;
	distance_message.vendorId = RD_VENDOR_ID;
	distance_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	distance_message.header = RD_OPCODE_CONFIG_SET_DISTANCE_RADA_SENSOR;
	distance_message.distance = distance;
	int rs = SendMessage(APP_REQ, (uint8_t *)&distance_message, sizeof(distance_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, distanceHeader, 0, 7);
	if (rs == CODE_OK)
	{
		return CODE_OK;
	}
	else
		LOGW("Set distance error");
	return CODE_ERROR;
}

int BleProtocol::SceneForScreenTouch(uint16_t devAddr, uint16_t scene, uint8_t icon, uint8_t type)
{
	LOGD("SceneForScreenTouch 0x%04X, scene %d, icon %d", devAddr, scene, icon);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t sceneScreenTouchHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t sceneId;
		uint8_t icon;
		uint8_t type;
	} scene_screen_touch_message_t;
	scene_screen_touch_message_t scene_screen_touch_message = {0};
	memset(&scene_screen_touch_message, 0x00, sizeof(scene_screen_touch_message));
	scene_screen_touch_message.ble_message_header.devAddr = devAddr;
	scene_screen_touch_message.opcodeVendor = RD_OPCODE_CONFIG;
	scene_screen_touch_message.vendorId = RD_VENDOR_ID;
	scene_screen_touch_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	scene_screen_touch_message.header = RD_OPCODE_CONFIG_SET_SCENE_SCREEN_TOUCH;
	scene_screen_touch_message.sceneId = scene;
	scene_screen_touch_message.icon = icon;
	scene_screen_touch_message.type = type;
	int rs = SendMessage(APP_REQ, (uint8_t *)&scene_screen_touch_message, sizeof(scene_screen_touch_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, sceneScreenTouchHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint16_t scene;
			uint8_t icon;
		} scene_screen_touch_rsp_message_t;
		scene_screen_touch_rsp_message_t *scene_screen_touch_rsp_message = (scene_screen_touch_rsp_message_t *)dataRsp;
		if (scene_screen_touch_rsp_message->header == 0x010a && scene_screen_touch_rsp_message->scene == scene && scene_screen_touch_rsp_message->icon == icon)
		{
			return CODE_OK;
		}
		LOGW("scene screen touch resp state not match with input control");
	}
	LOGW("scene screen touch err");
	return CODE_ERROR;
}

int BleProtocol::EditIconScreenTouch(uint16_t devAddr, uint16_t scene, uint8_t icon)
{
	LOGD("EditIconScreenTouch 0x%04x, scene %d, icon %d", devAddr, scene, icon);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t editIconScreenTouchHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t sceneId;
		uint8_t icon;
	} edit_icon_screen_touch_message_t;
	edit_icon_screen_touch_message_t edit_icon_screen_touch_message = {0};
	memset(&edit_icon_screen_touch_message, 0x00, sizeof(edit_icon_screen_touch_message));
	edit_icon_screen_touch_message.ble_message_header.devAddr = devAddr;
	edit_icon_screen_touch_message.opcodeVendor = RD_OPCODE_CONFIG;
	edit_icon_screen_touch_message.vendorId = RD_VENDOR_ID;
	edit_icon_screen_touch_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	edit_icon_screen_touch_message.header = RD_OPCODE_CONFIG_EDIT_ICON_SCREEN_TOUCH;
	edit_icon_screen_touch_message.sceneId = scene;
	edit_icon_screen_touch_message.icon = icon;
	int rs = SendMessage(APP_REQ, (uint8_t *)&edit_icon_screen_touch_message, sizeof(edit_icon_screen_touch_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, editIconScreenTouchHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint16_t scene;
			uint8_t icon;
		} scene_screen_touch_rsp_message_t;
		scene_screen_touch_rsp_message_t *scene_screen_touch_rsp_message = (scene_screen_touch_rsp_message_t *)dataRsp;
		if (scene_screen_touch_rsp_message->header == 0x070a && scene_screen_touch_rsp_message->scene == scene && scene_screen_touch_rsp_message->icon == icon)
		{
			return CODE_OK;
		}
		LOGW("edit icon screen touch resp state not match with input control");
	}
	LOGW("edit icon screen touch err");
	return CODE_ERROR;
}

int BleProtocol::DelSceneScreenTouch(uint16_t devAddr, uint16_t scene)
{
	LOGD("DelSceneScreenTouch 0x%04x, scene %d", devAddr, scene);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t delSceneScreenTouchHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t sceneId;
	} del_scene_screen_touch_message_t;
	del_scene_screen_touch_message_t del_scene_screen_touch_message = {0};
	memset(&del_scene_screen_touch_message, 0x00, sizeof(del_scene_screen_touch_message));
	del_scene_screen_touch_message.ble_message_header.devAddr = devAddr;
	del_scene_screen_touch_message.opcodeVendor = RD_OPCODE_CONFIG;
	del_scene_screen_touch_message.vendorId = RD_VENDOR_ID;
	del_scene_screen_touch_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	del_scene_screen_touch_message.header = RD_OPCODE_CONFIG_DEL_SCENE_SCREEN_TOUCH;
	del_scene_screen_touch_message.sceneId = scene;
	int rs = SendMessage(APP_REQ, (uint8_t *)&del_scene_screen_touch_message, sizeof(del_scene_screen_touch_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, delSceneScreenTouchHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint16_t scene;
		} scene_screen_touch_rsp_message_t;
		scene_screen_touch_rsp_message_t *scene_screen_touch_rsp_message = (scene_screen_touch_rsp_message_t *)dataRsp;
		if (scene_screen_touch_rsp_message->header == 0x020a && scene_screen_touch_rsp_message->scene == scene)
		{
			return CODE_OK;
		}
		LOGW("del scene screen touch resp state not match with input control");
	}
	LOGW("del scene screen touch err");
	return CODE_ERROR;
}

int BleProtocol::DelAllScene(uint16_t devAddr)
{
	LOGD("DelAllScene 0x%04x", devAddr);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t delAllSceneScreenTouchHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t check[6];
	} del_allscene_screen_touch_message_t;
	del_allscene_screen_touch_message_t del_allscene_screen_touch_message = {0};
	memset(&del_allscene_screen_touch_message, 0x00, sizeof(del_allscene_screen_touch_message));
	del_allscene_screen_touch_message.ble_message_header.devAddr = devAddr;
	del_allscene_screen_touch_message.opcodeVendor = RD_OPCODE_CONFIG;
	del_allscene_screen_touch_message.vendorId = RD_VENDOR_ID;
	del_allscene_screen_touch_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	del_allscene_screen_touch_message.header = RD_OPCODE_CONFIG_DEL_ALL_SCENE;
	for (int i = 0; i < 6; i++)
	{
		del_allscene_screen_touch_message.check[i] = i;
	}
	int rs = SendMessage(APP_REQ, (uint8_t *)&del_allscene_screen_touch_message, sizeof(del_allscene_screen_touch_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, delAllSceneScreenTouchHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
		} scene_screen_touch_rsp_message_t;
		scene_screen_touch_rsp_message_t *scene_screen_touch_rsp_message = (scene_screen_touch_rsp_message_t *)dataRsp;
		if (scene_screen_touch_rsp_message->header == 0x0a0a)
		{
			return CODE_OK;
		}
		LOGW("del all scene screen touch resp state not match with input control");
	}
	LOGW("del all scene screen touch err");
	return CODE_ERROR;
}

int BleProtocol::SendWeatherOutdoor(uint16_t devAddr, uint8_t status, uint16_t temp)
{
	LOGD("SendWeatherOutdoor 0x%04x, status %d, temp %d", devAddr, status, temp);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t weatherOutdoorScreenTouchHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t temp;
		uint8_t status;
	} weather_outdoor_screen_touch_message_t;
	weather_outdoor_screen_touch_message_t weather_outdoor_screen_touch_message = {0};
	memset(&weather_outdoor_screen_touch_message, 0x00, sizeof(weather_outdoor_screen_touch_message));
	weather_outdoor_screen_touch_message.ble_message_header.devAddr = devAddr;
	weather_outdoor_screen_touch_message.opcodeVendor = RD_OPCODE_CONFIG;
	weather_outdoor_screen_touch_message.vendorId = RD_VENDOR_ID;
	weather_outdoor_screen_touch_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	weather_outdoor_screen_touch_message.header = RD_OPCODE_CONFIG_SEND_WEATHER_OUTDOOR;
	weather_outdoor_screen_touch_message.temp = bswap_16(temp);
	weather_outdoor_screen_touch_message.status = status;
	int rs = SendMessage(APP_REQ, (uint8_t *)&weather_outdoor_screen_touch_message, sizeof(weather_outdoor_screen_touch_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1500, weatherOutdoorScreenTouchHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint16_t temp;
			uint8_t status;
		} weather_outdoor_screen_touch_rsp_message_t;
		weather_outdoor_screen_touch_rsp_message_t *weather_outdoor_screen_touch_rsp_message = (weather_outdoor_screen_touch_rsp_message_t *)dataRsp;
		if (weather_outdoor_screen_touch_rsp_message->header == 0x050a && weather_outdoor_screen_touch_rsp_message->temp == temp && weather_outdoor_screen_touch_rsp_message->status == status)
		{
			return CODE_OK;
		}
		LOGW("weather outdoor screen touch resp state not match with input control");
	}
	LOGW("weather outdoor screen touch err");
	return CODE_ERROR;
}

int BleProtocol::SendWeatherIndoor(uint16_t devAddr, uint16_t temp, uint16_t hum, uint16_t pm25)
{
	LOGD("SendWeatherIndoor 0x%04x, temp %d, hum %d", devAddr, temp, hum);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t weatherIndoorScreenTouchHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t temp;
		uint16_t hum;
		uint16_t pm25;
	} weather_indoor_screen_touch_message_t;
	weather_indoor_screen_touch_message_t weather_indoor_screen_touch_message = {0};
	memset(&weather_indoor_screen_touch_message, 0x00, sizeof(weather_indoor_screen_touch_message));
	weather_indoor_screen_touch_message.ble_message_header.devAddr = devAddr;
	weather_indoor_screen_touch_message.opcodeVendor = RD_OPCODE_CONFIG;
	weather_indoor_screen_touch_message.vendorId = RD_VENDOR_ID;
	weather_indoor_screen_touch_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	weather_indoor_screen_touch_message.header = 0x030a;
	weather_indoor_screen_touch_message.temp = bswap_16(temp);
	weather_indoor_screen_touch_message.hum = bswap_16(hum);
	weather_indoor_screen_touch_message.pm25 = pm25;
	int rs = SendMessage(APP_REQ, (uint8_t *)&weather_indoor_screen_touch_message, sizeof(weather_indoor_screen_touch_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1500, weatherIndoorScreenTouchHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint16_t temp;
			uint16_t hum;
			uint16_t pm25;
		} weather_outdoor_screen_touch_rsp_message_t;
		weather_outdoor_screen_touch_rsp_message_t *weather_outdoor_screen_touch_rsp_message = (weather_outdoor_screen_touch_rsp_message_t *)dataRsp;
		if (weather_outdoor_screen_touch_rsp_message->header == 0x030a && weather_outdoor_screen_touch_rsp_message->temp == temp && weather_outdoor_screen_touch_rsp_message->hum == hum)
		{
			return CODE_OK;
		}
		LOGW("weather indoor screen touch resp state not match with input control");
	}
	LOGW("weather indoor screen touch err");
	return CODE_ERROR;
}

int BleProtocol::SendDate(uint16_t devAddr, uint16_t years, uint8_t month, uint8_t date, uint8_t day)
{
	LOGD("SendDate 0x%04x, years %d, month %d, date %d, day %d", devAddr, years, month, date, day);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t dateScreenTouchHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t years;
		uint8_t month;
		uint8_t date;
		uint8_t day;
	} date_screen_touch_message_t;
	date_screen_touch_message_t date_screen_touch_message = {0};
	memset(&date_screen_touch_message, 0x00, sizeof(date_screen_touch_message));
	date_screen_touch_message.ble_message_header.devAddr = devAddr;
	date_screen_touch_message.opcodeVendor = RD_OPCODE_CONFIG;
	date_screen_touch_message.vendorId = RD_VENDOR_ID;
	date_screen_touch_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	date_screen_touch_message.header = RD_OPCODE_CONFIG_SEND_DATE;
	date_screen_touch_message.years = bswap_16(years);
	date_screen_touch_message.month = month;
	date_screen_touch_message.date = date;
	date_screen_touch_message.day = day;
	int rs = SendMessage(APP_REQ, (uint8_t *)&date_screen_touch_message, sizeof(date_screen_touch_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1500, dateScreenTouchHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint16_t years;
			uint8_t month;
			uint8_t date;
			uint8_t day;
		} date_screen_touch_rsp_message_t;
		date_screen_touch_rsp_message_t *date_screen_touch_rsp_message = (date_screen_touch_rsp_message_t *)dataRsp;
		if (date_screen_touch_rsp_message->header == 0x080a && date_screen_touch_rsp_message->years == bswap_16(years) && date_screen_touch_rsp_message->month == month && date_screen_touch_rsp_message->month == month && date_screen_touch_rsp_message->date == date && date_screen_touch_rsp_message->day == day)
		{
			return CODE_OK;
		}
		LOGW("date screen touch resp state not match with input control");
	}
	LOGW("date screen touch err");
	return CODE_ERROR;
}
int BleProtocol::SendTime(uint16_t devAddr, uint8_t hours, uint8_t minute, uint8_t second)
{
	LOGD("SendTime 0x%04x, hours %d, minute %d, second %d", devAddr, hours, minute, second);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t timeScreenTouchHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t hours;
		uint8_t minute;
		uint8_t second;
	} time_screen_touch_message_t;
	time_screen_touch_message_t time_screen_touch_message = {0};
	memset(&time_screen_touch_message, 0x00, sizeof(time_screen_touch_message));
	time_screen_touch_message.ble_message_header.devAddr = devAddr;
	time_screen_touch_message.opcodeVendor = RD_OPCODE_CONFIG;
	time_screen_touch_message.vendorId = RD_VENDOR_ID;
	time_screen_touch_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	time_screen_touch_message.header = RD_OPCODE_CONFIG_SEND_TIME;
	time_screen_touch_message.hours = hours;
	time_screen_touch_message.minute = minute;
	time_screen_touch_message.second = second;
	int rs = SendMessage(APP_REQ, (uint8_t *)&time_screen_touch_message, sizeof(time_screen_touch_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1500, timeScreenTouchHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint8_t hours;
			uint8_t minute;
			uint8_t second;
		} date_screen_touch_rsp_message_t;
		date_screen_touch_rsp_message_t *date_screen_touch_rsp_message = (date_screen_touch_rsp_message_t *)dataRsp;
		if (date_screen_touch_rsp_message->header == 0x090a && date_screen_touch_rsp_message->hours == hours && date_screen_touch_rsp_message->minute == minute && date_screen_touch_rsp_message->second == second)
		{
			return CODE_OK;
		}
		LOGW("time screen touch resp state not match with input control");
	}
	LOGW("time screen touch err");
	return CODE_ERROR;
}
int BleProtocol::SetGroup(uint16_t devAddr, uint16_t group)
{
	LOGD("SetGroup 0x%04x, group %d", devAddr, group);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t groupScreenTouchHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t group;
	} group_screen_touch_message_t;
	group_screen_touch_message_t group_screen_touch_message = {0};
	memset(&group_screen_touch_message, 0x00, sizeof(group_screen_touch_message));
	group_screen_touch_message.ble_message_header.devAddr = devAddr;
	group_screen_touch_message.opcodeVendor = RD_OPCODE_CONFIG;
	group_screen_touch_message.vendorId = RD_VENDOR_ID;
	group_screen_touch_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	group_screen_touch_message.header = RD_OPCODE_CONFIG_SET_GROUP;
	group_screen_touch_message.group = bswap_16(group);
	int rs = SendMessage(APP_REQ, (uint8_t *)&group_screen_touch_message, sizeof(group_screen_touch_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, groupScreenTouchHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint16_t group;
		} group_screen_touch_rsp_message_t;
		group_screen_touch_rsp_message_t *group_screen_touch_rsp_message = (group_screen_touch_rsp_message_t *)dataRsp;
		if (group_screen_touch_rsp_message->header == 0x0b0a && group_screen_touch_rsp_message->group == bswap_16(group))
		{
			return CODE_OK;
		}
		LOGW("group screen touch resp state not match with input control");
	}
	LOGW("group screen touch err");
	return CODE_ERROR;
}

int BleProtocol::ControlOpenClosePausePercent(uint16_t devAddr, uint8_t type, uint8_t percent)
{
	LOGD("ControlOpenClosePausePercent 0x%04x, type %d, percent %d", devAddr, type, percent);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t controlHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t type;
		uint8_t percent;
		uint8_t future[4];
	} control_message_t;
	control_message_t control_message = {0};
	memset(&control_message, 0x00, sizeof(control_message));
	control_message.ble_message_header.devAddr = devAddr;
	control_message.opcodeVendor = RD_OPCODE_CONFIG;
	control_message.vendorId = RD_VENDOR_ID;
	control_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	control_message.header = RD_OPCODE_CONTROL_OPEN_CLOSE_PAUSE;
	control_message.type = type;
	control_message.percent = percent;
	int rs = SendMessage(APP_REQ, (uint8_t *)&control_message, sizeof(control_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, controlHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint8_t type;
			uint8_t percent;
		} control_rsp_message_t;
		control_rsp_message_t *control_rsp_message = (control_rsp_message_t *)dataRsp;
		if (control_rsp_message->header == RD_OPCODE_RSP_CONTROL_OPEN_CLOSE_PAUSE_OPENED && control_rsp_message->type == type)
		{
			if (type == PERCENT)
			{
				if (control_rsp_message->percent == percent)
				{
					return CODE_OK;
				}
				LOGW("control resp opened error");
				return CODE_ERROR;
			}
			else
			{
				return CODE_OK;
			}
		}
		LOGW("control resp state not match with input control");
	}
	LOGW("control err");
	return CODE_ERROR;
}
int BleProtocol::ConfigMotor(uint16_t devAddr, uint8_t typeMotor)
{
	LOGD("ConfigMotor 0x%04x, typeMotor %d", devAddr, typeMotor);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t configHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t type;
		uint8_t future[5];
	} config_message_t;
	config_message_t config_message = {0};
	memset(&config_message, 0x00, sizeof(config_message));
	config_message.ble_message_header.devAddr = devAddr;
	config_message.opcodeVendor = RD_OPCODE_CONFIG;
	config_message.vendorId = RD_VENDOR_ID;
	config_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	config_message.header = RD_OPCODE_CONFIG_MOTOR;
	config_message.type = typeMotor;
	int rs = SendMessage(APP_REQ, (uint8_t *)&config_message, sizeof(config_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, configHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint8_t type;
		} config_rsp_message_t;
		config_rsp_message_t *config_rsp_message = (config_rsp_message_t *)dataRsp;
		if (config_rsp_message->header == RD_OPCODE_CONFIG_MOTOR && config_rsp_message->type == typeMotor)
		{
			return CODE_OK;
		}
		LOGW("config resp state not match with input control");
	}
	LOGW("config err");
	return CODE_ERROR;
}
int BleProtocol::CalibCurtain(uint16_t devAddr, uint8_t status)
{
	LOGD("CalibCurtain 0x%04x, status %d", devAddr, status);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t calibHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t status;
		uint8_t future[5];
	} calib_message_t;
	calib_message_t calib_message = {0};
	memset(&calib_message, 0x00, sizeof(calib_message));
	calib_message.ble_message_header.devAddr = devAddr;
	calib_message.opcodeVendor = RD_OPCODE_CONFIG;
	calib_message.vendorId = RD_VENDOR_ID;
	calib_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	calib_message.header = RD_OPCODE_CALIB;
	calib_message.status = status;
	int rs = SendMessage(APP_REQ, (uint8_t *)&calib_message, sizeof(calib_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, calibHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint8_t status;
		} calib_rsp_message_t;
		calib_rsp_message_t *calib_rsp_message = (calib_rsp_message_t *)dataRsp;
		if (calib_rsp_message->header == RD_OPCODE_CALIB && calib_rsp_message->status == status)
		{
			return CODE_OK;
		}
		LOGW("calib resp state not match with input control");
	}
	LOGW("calib err");
	return CODE_ERROR;
}

int BleProtocol::UpdateStatusCurtain(uint16_t devAddr)
{
	LOGD("UpdateStatusCurtain 0x%04x", devAddr);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t updateHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t future[6];
	} update_message_t;
	update_message_t update_message = {0};
	memset(&update_message, 0x00, sizeof(update_message));
	update_message.ble_message_header.devAddr = devAddr;
	update_message.opcodeVendor = RD_OPCODE_CONFIG;
	update_message.vendorId = RD_VENDOR_ID;
	update_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	update_message.header = RD_OPCODE_REQUEST_STATUS_CURTAIN;
	int rs = SendMessage(APP_REQ, (uint8_t *)&update_message, sizeof(update_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, updateHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
		} calib_rsp_message_t;
		calib_rsp_message_t *calib_rsp_message = (calib_rsp_message_t *)dataRsp;
		if (calib_rsp_message->header == RD_OPCODE_REQUEST_STATUS_CURTAIN)
		{
			return CODE_OK;
		}
		LOGW("update resp state not match with input control");
	}
	LOGW("update err");
	return CODE_ERROR;
}

int BleProtocol::ScanStopSeftPowerRemote(uint16_t devAddr, uint8_t status)
{
	LOGD("Scan SeftPowerRemote: 0x%04X, status: %d", devAddr, status);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t dataCompare[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t status;
		uint8_t future[5];
	} scan_message_t;
	scan_message_t scan_message = {0};
	memset(&scan_message, 0x00, sizeof(scan_message));
	scan_message.ble_message_header.devAddr = devAddr;
	scan_message.opcodeVendor = RD_OPCODE_CONFIG;
	scan_message.vendorId = RD_VENDOR_ID;
	scan_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	scan_message.header = RD_OPCODE_SEFTPOWER_REMOTE_SCAN;
	scan_message.status = status;
	uint16_t timeout = 10000;
	if (!status)
		timeout = 1000;
	int rs = SendMessage(APP_REQ, (uint8_t *)&scan_message, sizeof(scan_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, timeout, dataCompare, 0, 7);
	if (rs == CODE_OK)
	{
		memcpy(&scanDevicePairMessage, dataRsp, sizeof(scan_device_pair_message_t));
		// LOGW("parent: %d", scanDevicePairMessage.parentAddr);
		// LOGW("mac: %02x%02x%02x%02x", scanDevicePairMessage.mac[0], scanDevicePairMessage.mac[1], scanDevicePairMessage.mac[2], scanDevicePairMessage.mac[3]);
		// LOGW("type: %d", scanDevicePairMessage.type);
	}
	return rs;
}

int BleProtocol::SaveSeftPowerRemote(scan_device_pair_message_t scanMessage, uint16_t childDev)
{
	LOGD("Save SeftPowerRemote");
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t dataCompare[] = {(uint8_t)(scanMessage.parentAddr & 0xFF), (uint8_t)((scanMessage.parentAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02, 0x0b, 0x0d};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t mac[4];
		uint16_t childDev;
	} save_message_t;
	save_message_t save_message = {0};
	memset(&save_message, 0x00, sizeof(save_message));
	save_message.ble_message_header.devAddr = scanMessage.parentAddr;
	save_message.opcodeVendor = RD_OPCODE_CONFIG;
	save_message.vendorId = RD_VENDOR_ID;
	save_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	save_message.header = RD_OPCODE_SEFTPOWER_REMOTE_SAVE;
	save_message.childDev = childDev;
	for (int i = 0; i < 4; i++)
	{
		save_message.mac[i] = scanMessage.mac[i];
	}
	int rs = SendMessage(APP_REQ, (uint8_t *)&save_message, sizeof(save_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 3000, dataCompare, 0, 9);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint8_t mac[4];
			uint8_t status;
		} save_rsp_message_t;
		save_rsp_message_t *save_rsp_message = (save_rsp_message_t *)dataRsp;
		uint32_t parentAddrRsp = save_rsp_message->mac[0] | save_rsp_message->mac[1] << 8 | save_rsp_message->mac[2] << 16 | save_rsp_message->mac[3] << 24;
		uint32_t parentAddr = scanMessage.mac[0] | scanMessage.mac[1] << 8 | scanMessage.mac[2] << 16 | scanMessage.mac[3] << 24;
		if ((parentAddrRsp != parentAddr) || (save_rsp_message->status == 0))
			rs = CODE_ERROR;
	}
	return rs;
}

int BleProtocol::SetSceneSeftPowerRemote(uint16_t devAddr, uint16_t seftPowerAddr, uint8_t button, uint8_t mode, uint16_t scene)
{
	LOGD("SetSceneSeftPowerRemote parent: 0x%04X, device: 0x%04X, button: %d, mode: %d, scene: %d", devAddr, seftPowerAddr, button, mode, scene);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t dataCompare[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t childDev;
		uint8_t button;
		uint8_t mode;
		uint16_t scene;
	} scene_message_t;
	scene_message_t scene_message = {0};
	memset(&scene_message, 0x00, sizeof(scene_message));
	scene_message.ble_message_header.devAddr = devAddr;
	scene_message.opcodeVendor = RD_OPCODE_CONFIG;
	scene_message.vendorId = RD_VENDOR_ID;
	scene_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	scene_message.header = RD_OPCODE_SEFTPOWER_REMOTE_SET_SCENE;
	scene_message.childDev = seftPowerAddr;
	scene_message.button = button;
	scene_message.mode = mode;
	scene_message.scene = scene;

	int rs = SendMessage(APP_REQ, (uint8_t *)&scene_message, sizeof(scene_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, dataCompare, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint8_t status;
		} scene_rsp_message_t;
		scene_rsp_message_t *scene_rsp_message = (scene_rsp_message_t *)dataRsp;
		if (scene_rsp_message->header != RD_OPCODE_SEFTPOWER_REMOTE_SET_SCENE)
			rs = CODE_ERROR;
	}
	return rs;
}

int BleProtocol::DelSceneSeftPowerRemote(uint16_t devAddr, uint16_t seftPowerAddr, uint8_t button, uint8_t mode)
{
	LOGD("DelSceneSeftPowerRemote parent: 0x%04X, device: 0x%04X, button: %d, mode: %d", devAddr, seftPowerAddr, button, mode);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t dataCompare[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t childDev;
		uint8_t button;
		uint8_t mode;
		uint16_t feature;
	} scene_message_t;
	scene_message_t scene_message = {0};
	memset(&scene_message, 0x00, sizeof(scene_message));
	scene_message.ble_message_header.devAddr = devAddr;
	scene_message.opcodeVendor = RD_OPCODE_CONFIG;
	scene_message.vendorId = RD_VENDOR_ID;
	scene_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	scene_message.header = RD_OPCODE_SEFTPOWER_REMOTE_DEL_SCENE;
	scene_message.childDev = seftPowerAddr;
	scene_message.button = button;
	scene_message.mode = mode;
	int rs = SendMessage(APP_REQ, (uint8_t *)&scene_message, sizeof(scene_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, dataCompare, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint8_t status;
		} scene_rsp_message_t;
		scene_rsp_message_t *scene_rsp_message = (scene_rsp_message_t *)dataRsp;
		if (scene_rsp_message->header != RD_OPCODE_SEFTPOWER_REMOTE_DEL_SCENE)
			rs = CODE_ERROR;
	}
	return rs;
}

int BleProtocol::ResetSeftPowerRemote(uint16_t devAddr, uint16_t seftPowerAddr)
{
	LOGD("ResetSeftPowerRemote parent: 0x%04X, device: 0x%04X", devAddr, seftPowerAddr);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t dataCompare[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t childDev;
		uint8_t feature[4];
	} reset_message_t;
	reset_message_t reset_message = {0};
	memset(&reset_message, 0x00, sizeof(reset_message));
	reset_message.ble_message_header.devAddr = devAddr;
	reset_message.opcodeVendor = RD_OPCODE_CONFIG;
	reset_message.vendorId = RD_VENDOR_ID;
	reset_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	reset_message.header = RD_OPCODE_SEFTPOWER_REMOTE_RESET;
	reset_message.childDev = seftPowerAddr;
	int rs = SendMessage(APP_REQ, (uint8_t *)&reset_message, sizeof(reset_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, dataCompare, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint16_t childAddr;
			uint8_t status;
		} reset_rsp_message_t;
		reset_rsp_message_t *reset_rsp_message = (reset_rsp_message_t *)dataRsp;
		if (reset_rsp_message->header != RD_OPCODE_SEFTPOWER_REMOTE_RESET)
			rs = CODE_ERROR;
	}
	return rs;
}

int BleProtocol::AddDeviceToRoom(uint16_t devAddr, uint16_t roomAddr)
{
	LOGD("AddDeviceToRoom 0x%04x, roomAddr %d", devAddr, roomAddr);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t addDevToRoomHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t roomAddr;
	} add_dev_to_room_message_t;
	add_dev_to_room_message_t add_dev_to_room_message = {0};
	memset(&add_dev_to_room_message, 0x00, sizeof(add_dev_to_room_message));
	add_dev_to_room_message.ble_message_header.devAddr = devAddr;
	add_dev_to_room_message.opcodeVendor = RD_OPCODE_CONFIG;
	add_dev_to_room_message.vendorId = RD_VENDOR_ID;
	add_dev_to_room_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	add_dev_to_room_message.header = 0x0b0a;
	add_dev_to_room_message.roomAddr = bswap_16(roomAddr);
	int rs = SendMessage(APP_REQ, (uint8_t *)&add_dev_to_room_message, 21, HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, addDevToRoomHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint16_t group;
		} group_screen_touch_rsp_message_t;
		group_screen_touch_rsp_message_t *group_screen_touch_rsp_message = (group_screen_touch_rsp_message_t *)dataRsp;
		if (group_screen_touch_rsp_message->header == 0x0b0a && group_screen_touch_rsp_message->group == bswap_16(roomAddr))
		{
			return CODE_OK;
		}
		LOGW("group screen touch resp state not match with input control");
	}
	LOGW("group screen touch err");
	return CODE_ERROR;
}

int BleProtocol::ControlRgbSwitch(uint16_t devAddr, uint8_t button, uint8_t b, uint8_t g, uint8_t r, uint8_t dimOn, uint8_t dimOff)
{
	LOGD("ControlRgbSwitch 0x%04X, button %d, r %d, g %d, b %d, dimon %d, dimOff %d", devAddr, button, r, g, b, dimOn, dimOff);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t controlRgbSwitchHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t button;
		uint8_t b;
		uint8_t g;
		uint8_t r;
		uint8_t dimOn;
		uint8_t dimOff;
	} controlrgb_switch_message_t;
	controlrgb_switch_message_t controlrgb_switch_message = {0};
	memset(&controlrgb_switch_message, 0x00, sizeof(controlrgb_switch_message));
	controlrgb_switch_message.ble_message_header.devAddr = devAddr;
	controlrgb_switch_message.opcodeVendor = RD_OPCODE_CONFIG;
	controlrgb_switch_message.vendorId = RD_VENDOR_ID;
	controlrgb_switch_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	controlrgb_switch_message.header = RD_OPCODE_CONFIG_CONTROL_RGB_SWITCH;
	controlrgb_switch_message.button = button;
	controlrgb_switch_message.b = b;
	controlrgb_switch_message.g = g;
	controlrgb_switch_message.r = r;
	controlrgb_switch_message.dimOn = dimOn;
	controlrgb_switch_message.dimOff = dimOff;
	int rs = SendMessage(APP_REQ, (uint8_t *)&controlrgb_switch_message, sizeof(controlrgb_switch_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, controlRgbSwitchHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint8_t button;
		} controlrgb_switch_rsp_message_t;
		controlrgb_switch_rsp_message_t *controlrgb_switch_rsp_message = (controlrgb_switch_rsp_message_t *)dataRsp;
		if (controlrgb_switch_rsp_message->header == 0x050b && controlrgb_switch_rsp_message->button == button)
		{
			return CODE_OK;
		}
		LOGW("control rgb switch resp state not match with input control");
	}
	LOGW("control rgb switch err");
	return CODE_ERROR;
}

int BleProtocol::ControlRelayOfSwitch(uint16_t devAddr, uint16_t type, uint8_t relay, uint8_t value)
{
	LOGD("ControlRelayOfSwitch 0x%04X, relayid %d, value %d", devAddr, relay, value);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t controlRelaySwitchHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t relay;
		uint8_t value;
	} control_relay_switch_message_t;
	control_relay_switch_message_t control_relay_switch_message = {0};
	memset(&control_relay_switch_message, 0x00, sizeof(control_relay_switch_message));
	control_relay_switch_message.ble_message_header.devAddr = devAddr;
	control_relay_switch_message.opcodeVendor = RD_OPCODE_CONFIG;
	control_relay_switch_message.vendorId = RD_VENDOR_ID;
	control_relay_switch_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	control_relay_switch_message.header = RD_OPCODE_CONFIG_CONTROL_RELAY_SWITCH_4;
	control_relay_switch_message.relay = relay;
	control_relay_switch_message.value = value;
	int rs = SendMessage(APP_REQ, (uint8_t *)&control_relay_switch_message, sizeof(control_relay_switch_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, controlRelaySwitchHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint8_t relay;
			uint8_t value;
		} control_relay_switch_rsp_message_t;
		control_relay_switch_rsp_message_t *control_relay_switch_rsp_message = (control_relay_switch_rsp_message_t *)dataRsp;
		if (control_relay_switch_rsp_message->header == 0x000b && control_relay_switch_rsp_message->relay == relay && control_relay_switch_rsp_message->value == value)
		{
			return CODE_OK;
		}
		LOGW("control relay switch resp state not match with input control");
	}
	LOGW("control relay switch err");
	return CODE_ERROR;
}

int BleProtocol::SetIdCombine(uint16_t devAddr, uint16_t id)
{
	LOGD("SetIdCombine 0x%04x, id %d", devAddr, id);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t setIdCombineHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint16_t id;
	} set_id_combine_message_t;
	set_id_combine_message_t set_id_combine_message = {0};
	memset(&set_id_combine_message, 0x00, sizeof(set_id_combine_message));
	set_id_combine_message.ble_message_header.devAddr = devAddr;
	set_id_combine_message.opcodeVendor = RD_OPCODE_CONFIG;
	set_id_combine_message.vendorId = RD_VENDOR_ID;
	set_id_combine_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	set_id_combine_message.header = RD_OPCODE_CONFIG_SET_ID_COMBINE;
	set_id_combine_message.id = id;
	int rs = SendMessage(APP_REQ, (uint8_t *)&set_id_combine_message, sizeof(set_id_combine_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 2000, setIdCombineHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint16_t id;
		} set_id_combine_rsp_message_t;
		set_id_combine_rsp_message_t *set_id_combine_rsp_message = (set_id_combine_rsp_message_t *)dataRsp;
		if (set_id_combine_rsp_message->header == 0x060b && set_id_combine_rsp_message->id == id)
		{
			return CODE_OK;
		}
		LOGW("set id combine resp state not match with input control");
	}
	LOGW("set id combine err");
	return CODE_ERROR;
}

int BleProtocol::CountDownSwitch(uint16_t devAddr, uint32_t timer, uint8_t status)
{
	LOGD("CountDownSwitch 0x%04x, timer %d, status %d", devAddr, timer, status);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t timerHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t status;
		uint32_t timer;
	} timer_message_t;
	timer_message_t timer_message = {0};
	memset(&timer_message, 0x00, sizeof(timer_message));
	timer_message.ble_message_header.devAddr = devAddr;
	timer_message.opcodeVendor = RD_OPCODE_CONFIG;
	timer_message.vendorId = RD_VENDOR_ID;
	timer_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	timer_message.header = RD_OPCODE_CONFIG_SET_TIMER;
	timer_message.status = status;
	timer_message.timer = bswap_32(timer);
	int rs = SendMessage(APP_REQ, (uint8_t *)&timer_message, sizeof(timer_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, timerHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint8_t status;
			uint8_t timer[4];
		} timer_rsp_message_t;
		timer_rsp_message_t *timer_rsp_message = (timer_rsp_message_t *)dataRsp;
		if (timer_rsp_message->header == 0x070b && timer_rsp_message->status == status)
		{
			return CODE_OK;
		}
		LOGW("CountDownSwitch resp state not match with input control");
	}
	LOGW("CountDownSwitch err");
	return CODE_ERROR;
}

int BleProtocol::UpdateStatusRelaySwitch(uint16_t devAddr, uint32_t type)
{
	LOGD("Update status Relay Switch 0x%04x", devAddr);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t timerHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
	} request_status_message_t;
	request_status_message_t request_status_message = {0};
	memset(&request_status_message, 0x00, sizeof(request_status_message));
	request_status_message.ble_message_header.devAddr = devAddr;
	request_status_message.opcodeVendor = RD_OPCODE_CONFIG;
	request_status_message.vendorId = RD_VENDOR_ID;
	request_status_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	request_status_message.header = RD_OPCODE_REQUEST_STATUS_SWITCH;
	int rs = SendMessage(APP_REQ, (uint8_t *)&request_status_message, sizeof(request_status_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, timerHeader, 0, 7);
	if (rs == CODE_OK)
	{
		return CODE_OK;
	}
	LOGW("request status switch err");
	return CODE_ERROR;
}

int BleProtocol::ConfigStatusStartupSwitch(uint16_t devAddr, uint8_t status, uint32_t type)
{
	LOGD("ConfigStatusStartup 0x%04x, status %d", devAddr, status);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t statusHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t status;
	} status_message_t;
	status_message_t status_message = {0};
	memset(&status_message, 0x00, sizeof(status_message));
	status_message.ble_message_header.devAddr = devAddr;
	status_message.opcodeVendor = RD_OPCODE_CONFIG;
	status_message.vendorId = RD_VENDOR_ID;
	status_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	status_message.header = RD_OPCODE_CONFIG_STATUS_STARTUP_SWITCH;
	status_message.status = status;
	int rs = SendMessage(APP_REQ, (uint8_t *)&status_message, sizeof(status_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, statusHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint8_t status;
		} status_rsp_message_t;
		status_rsp_message_t *status_rsp_message = (status_rsp_message_t *)dataRsp;
		if (status_rsp_message->header == RD_OPCODE_CONFIG_STATUS_STARTUP_SWITCH && status_rsp_message->status == status)
		{
			return CODE_OK;
		}
		LOGW("status startup resp state not match with input control");
	}
	LOGW("status startup switch err");
	return CODE_ERROR;
}

int BleProtocol::ConfigModeInputSwitchOnoff(uint16_t devAddr, uint8_t mode)
{
	LOGD("ConfigModeInputStatusOnoff 0x%04x, mode %d", devAddr, mode);
	uint8_t dataRsp[100];
	int lenRsp;
	uint8_t modeHeader[] = {(uint8_t)(devAddr & 0xFF), (uint8_t)((devAddr >> 8) & 0xFF), 1, 0, 0xe3, 0x11, 0x02};
	typedef struct __attribute__((packed))
	{
		ble_message_header_t ble_message_header;
		uint8_t opcodeVendor;
		uint16_t vendorId;
		uint8_t opcodeRsp;
		uint8_t tidPos;
		uint16_t header;
		uint8_t mode;
	} mode_message_t;
	mode_message_t mode_message = {0};
	memset(&mode_message, 0x00, sizeof(mode_message));
	mode_message.ble_message_header.devAddr = devAddr;
	mode_message.opcodeVendor = RD_OPCODE_CONFIG;
	mode_message.vendorId = RD_VENDOR_ID;
	mode_message.opcodeRsp = RD_OPCODE_CONFIG_RSP;
	mode_message.header = RD_OPCODE_CONFIG_MODE_INPUT_SWITCHONOFF;
	mode_message.mode = mode;
	int rs = SendMessage(APP_REQ, (uint8_t *)&mode_message, sizeof(mode_message_t), HCI_GATEWAY_RSP_OP_CODE, dataRsp, &lenRsp, 1000, modeHeader, 0, 7);
	if (rs == CODE_OK)
	{
		typedef struct __attribute__((packed))
		{
			uint16_t devAddr;
			uint16_t gwAddr;
			uint8_t opcodeRsp;
			uint16_t vendorId;
			uint16_t header;
			uint8_t mode;
		} mode_rsp_message_t;
		mode_rsp_message_t *mode_rsp_message = (mode_rsp_message_t *)dataRsp;
		if (mode_rsp_message->header == RD_OPCODE_CONFIG_MODE_INPUT_SWITCHONOFF && mode_rsp_message->mode == mode)
		{
			return CODE_OK;
		}
		LOGW("mode input resp state not match with input control");
	}
	LOGW("mode input switch err");
	return CODE_ERROR;
}

int BleProtocol::UpdateAppKey(string appKey)
{
	LOGD("UpdateAppKey %s", appKey.c_str());
	appKey.erase(std::remove(appKey.begin(), appKey.end(), '-'), appKey.end());
	if (appKey.size() == 32)
	{
		uint8_t dataRsp[100];
		int lenRsp;
		typedef struct __attribute__((packed))
		{
			uint8_t opcode;
			uint8_t rev[3];
			uint8_t appKey[16];
		} binding_all_message_t;
		binding_all_message_t binding_all_message;
		memset(&binding_all_message, 0x00, sizeof(binding_all_message));
		binding_all_message.opcode = HCI_GATEWAY_CMD_START_KEYBIND;
		for (int i = 0; i < appKey.length(); i += 2)
		{
			std::string hexByte = appKey.substr(i, 2);
			binding_all_message.appKey[i / 2] = std::stoi(hexByte, nullptr, 16);
		}
		return SendMessage(SYSTEM_REQ, (uint8_t *)&binding_all_message, sizeof(binding_all_message_t), HCI_GATEWAY_CMD_KEY_BIND_EVT, dataRsp, &lenRsp, 30000);
	}
	else
	{
		LOGW("UpdateAppKey error");
	}
	return CODE_ERROR;
}
