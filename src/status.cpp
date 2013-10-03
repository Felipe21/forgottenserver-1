/**
 * The Forgotten Server - a server application for the MMORPG Tibia
 * Copyright (C) 2013  Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "otpch.h"

#include "status.h"
#include "configmanager.h"
#include "game.h"
#include "connection.h"
#include "networkmessage.h"
#include "outputmessage.h"
#include "tools.h"

extern ConfigManager g_config;
extern Game g_game;

#ifdef __ENABLE_SERVER_DIAGNOSTIC__
uint32_t ProtocolStatus::protocolStatusCount = 0;
#endif

enum RequestedInfo_t {
	REQUEST_BASIC_SERVER_INFO = 0x01,
	REQUEST_OWNER_SERVER_INFO = 0x02,
	REQUEST_MISC_SERVER_INFO = 0x04,
	REQUEST_PLAYERS_INFO = 0x08,
	REQUEST_MAP_INFO = 0x10,
	REQUEST_EXT_PLAYERS_INFO = 0x20,
	REQUEST_PLAYER_STATUS_INFO = 0x40,
	REQUEST_SERVER_SOFTWARE_INFO = 0x80
};

std::map<uint32_t, int64_t> ProtocolStatus::ipConnectMap;

void ProtocolStatus::onRecvFirstMessage(NetworkMessage& msg)
{
	uint32_t ip = getIP();

	if (ip != 0x0100007F) {
		std::string ipStr = convertIPToString(ip);

		if (ipStr != g_config.getString(ConfigManager::IP)) {
			std::map<uint32_t, int64_t>::const_iterator it = ipConnectMap.find(ip);

			if (it != ipConnectMap.end()) {
				if (OTSYS_TIME() < (it->second + g_config.getNumber(ConfigManager::STATUSQUERY_TIMEOUT))) {
					getConnection()->closeConnection();
					return;
				}
			}
		}
	}

	ipConnectMap[ip] = OTSYS_TIME();

	switch (msg.GetByte()) {
		//XML info protocol
		case 0xFF: {
			if (msg.GetString(4) == "info") {
				OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false);
				if (output) {
					Status* status = Status::getInstance();
					std::string str = status->getStatusString();
					output->AddBytes(str.c_str(), str.size());
					setRawMessages(true); // we dont want the size header, nor encryption
					OutputMessagePool::getInstance()->send(output);
				}
			}

			break;
		}

		//Another ServerInfo protocol
		case 0x01: {
			uint32_t requestedInfo = msg.GetU16(); //Only a Byte is necessary, though we could add new infos here
			OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false);
			if (output) {
				Status* status = Status::getInstance();
				status->getInfo(requestedInfo, output, msg);
				OutputMessagePool::getInstance()->send(output);
			}

			break;
		}
		default:
			break;
	}

	getConnection()->closeConnection();
}

Status::Status()
{
	m_playersOnline = 0;
	m_start = OTSYS_TIME();
}

void Status::addPlayer()
{
	m_playersOnline++;
}

void Status::removePlayer()
{
	m_playersOnline--;
}

std::string Status::getStatusString() const
{
	pugi::xml_document doc;

	pugi::xml_node decl = doc.prepend_child(pugi::node_declaration);
	decl.append_attribute("version") = "1.0";

	pugi::xml_node tsqp = doc.append_child("tsqp");
	tsqp.append_attribute("version") = "1.0";

	pugi::xml_node serverinfo = tsqp.append_child("serverinfo");
	serverinfo.append_attribute("uptime") = std::to_string(getUptime()).c_str();
	serverinfo.append_attribute("ip") = g_config.getString(ConfigManager::IP).c_str();
	serverinfo.append_attribute("servername") = g_config.getString(ConfigManager::SERVER_NAME).c_str();
	serverinfo.append_attribute("port") = std::to_string(g_config.getNumber(ConfigManager::LOGIN_PORT)).c_str();
	serverinfo.append_attribute("location") = g_config.getString(ConfigManager::LOCATION).c_str();
	serverinfo.append_attribute("url") = g_config.getString(ConfigManager::URL).c_str();
	serverinfo.append_attribute("server") = STATUS_SERVER_NAME;
	serverinfo.append_attribute("version") = STATUS_SERVER_VERSION;
	serverinfo.append_attribute("client") = STATUS_SERVER_PROTOCOL;

	pugi::xml_node owner = tsqp.append_child("owner");
	owner.append_attribute("name") = g_config.getString(ConfigManager::OWNER_NAME).c_str();
	owner.append_attribute("email") = g_config.getString(ConfigManager::OWNER_EMAIL).c_str();

	pugi::xml_node players = tsqp.append_child("players");
	players.append_attribute("online") = std::to_string(m_playersOnline).c_str();
	players.append_attribute("max") = std::to_string(g_config.getNumber(ConfigManager::MAX_PLAYERS)).c_str();
	players.append_attribute("peak") = std::to_string(g_game.getPlayersRecord()).c_str();

	pugi::xml_node monsters = tsqp.append_child("monsters");
	monsters.append_attribute("total") = std::to_string(g_game.getMonstersOnline()).c_str();

	pugi::xml_node map = tsqp.append_child("map");
	map.append_attribute("name") = g_config.getString(ConfigManager::MAP_NAME).c_str();
	map.append_attribute("author") = g_config.getString(ConfigManager::MAP_AUTHOR).c_str();

	uint32_t mapWidth, mapHeight;
	g_game.getMapDimensions(mapWidth, mapHeight);
	map.append_attribute("width") = std::to_string(mapWidth).c_str();
	map.append_attribute("height") = std::to_string(mapHeight).c_str();

	pugi::xml_node motd = tsqp.append_child("motd");
	motd.text() = g_config.getString(ConfigManager::MOTD).c_str();

	std::ostringstream ss;
	doc.save(ss, "", pugi::format_raw);
	return ss.str();
}

void Status::getInfo(uint32_t requestedInfo, OutputMessage_ptr output, NetworkMessage& msg) const
{
	if (requestedInfo & REQUEST_BASIC_SERVER_INFO) {
		output->AddByte(0x10);
		output->AddString(g_config.getString(ConfigManager::SERVER_NAME));
		output->AddString(g_config.getString(ConfigManager::IP));
		output->AddString(std::to_string(g_config.getNumber(ConfigManager::LOGIN_PORT)));
	}

	if (requestedInfo & REQUEST_OWNER_SERVER_INFO) {
		output->AddByte(0x11);
		output->AddString(g_config.getString(ConfigManager::OWNER_NAME));
		output->AddString(g_config.getString(ConfigManager::OWNER_EMAIL));
	}

	if (requestedInfo & REQUEST_MISC_SERVER_INFO) {
		uint64_t running = getUptime();
		output->AddByte(0x12);
		output->AddString(g_config.getString(ConfigManager::MOTD));
		output->AddString(g_config.getString(ConfigManager::LOCATION));
		output->AddString(g_config.getString(ConfigManager::URL));
		output->AddU32((uint32_t)(running >> 32));
		output->AddU32((uint32_t)(running));
	}

	if (requestedInfo & REQUEST_PLAYERS_INFO) {
		output->AddByte(0x20);
		output->AddU32(m_playersOnline);
		output->AddU32(g_config.getNumber(ConfigManager::MAX_PLAYERS));
		output->AddU32(g_game.getPlayersRecord());
	}

	if (requestedInfo & REQUEST_MAP_INFO) {
		output->AddByte(0x30);
		output->AddString(g_config.getString(ConfigManager::MAP_NAME));
		output->AddString(g_config.getString(ConfigManager::MAP_AUTHOR));
		uint32_t mapWidth, mapHeight;
		g_game.getMapDimensions(mapWidth, mapHeight);
		output->AddU16(mapWidth);
		output->AddU16(mapHeight);
	}

	if (requestedInfo & REQUEST_EXT_PLAYERS_INFO) {
		output->AddByte(0x21); // players info - online players list
		output->AddU32(m_playersOnline);

		for (const auto& it : g_game.getPlayers()) {
			output->AddString(it.second->getName());
			output->AddU32(it.second->getLevel());
		}
	}

	if (requestedInfo & REQUEST_PLAYER_STATUS_INFO) {
		output->AddByte(0x22); // players info - online status info of a player
		const std::string name = msg.GetString();

		if (g_game.getPlayerByName(name) != nullptr) {
			output->AddByte(0x01);
		} else {
			output->AddByte(0x00);
		}
	}

	if (requestedInfo & REQUEST_SERVER_SOFTWARE_INFO) {
		output->AddByte(0x23); // server software info
		output->AddString(STATUS_SERVER_NAME);
		output->AddString(STATUS_SERVER_VERSION);
		output->AddString(STATUS_SERVER_PROTOCOL);
	}
}

uint64_t Status::getUptime() const
{
	return (OTSYS_TIME() - m_start) / 1000;
}
