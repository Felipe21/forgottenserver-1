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

#include "protocolgame.h"

#include "networkmessage.h"
#include "outputmessage.h"

#include "items.h"

#include "tile.h"
#include "player.h"
#include "chat.h"

#include "configmanager.h"
#include "actions.h"
#include "game.h"
#include "iologindata.h"
#include "house.h"
#include "waitlist.h"
#include "ban.h"
#include "connection.h"
#include "creatureevent.h"

#include <ctime>
#include <list>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <random>

#include <boost/function.hpp>

extern Game g_game;
extern ConfigManager g_config;
extern Actions actions;
extern Ban g_bans;
extern CreatureEvents* g_creatureEvents;
Chat g_chat;

#ifdef __ENABLE_SERVER_DIAGNOSTIC__
uint32_t ProtocolGame::protocolGameCount = 0;
#endif

// Helping templates to add dispatcher tasks
template<class FunctionType>
void ProtocolGame::addGameTaskInternal(bool droppable, uint32_t delay, const FunctionType& func)
{
	if (droppable) {
		g_dispatcher.addTask(createTask(delay, func));
	} else {
		g_dispatcher.addTask(createTask(func));
	}
}

ProtocolGame::ProtocolGame(Connection_ptr connection) :
	Protocol(connection),
	player(nullptr),
	eventConnect(0),
	// version(CLIENT_VERSION_MIN),
	m_debugAssertSent(false),
	m_acceptPackets(false)
{
#ifdef __ENABLE_SERVER_DIAGNOSTIC__
	protocolGameCount++;
#endif
}

ProtocolGame::~ProtocolGame()
{
	player = nullptr;
#ifdef __ENABLE_SERVER_DIAGNOSTIC__
	protocolGameCount--;
#endif
}

void ProtocolGame::setPlayer(Player* p)
{
	player = p;
}

void ProtocolGame::releaseProtocol()
{
	//dispatcher thread
	if (player && player->client == this) {
		player->client = nullptr;
	}
	Protocol::releaseProtocol();
}

void ProtocolGame::deleteProtocolTask()
{
	//dispatcher thread
	if (player) {
		g_game.ReleaseCreature(player);
		player = nullptr;
	}

	Protocol::deleteProtocolTask();
}

bool ProtocolGame::login(const std::string& name, uint32_t accountId, OperatingSystem_t operatingSystem, bool gamemasterLogin)
{
	//dispatcher thread
	Player* _player = g_game.getPlayerByName(name);
	if (!_player || g_config.getBoolean(ConfigManager::ALLOW_CLONES)) {
		player = new Player(this);
		player->setName(name);

		player->useThing2();
		player->setID();

		if (!IOLoginData::getInstance()->preloadPlayer(player, name)) {
			disconnectClient(0x14, "Your character could not be loaded.");
			return false;
		}

		if (IOBan::getInstance()->isPlayerNamelocked(player->getGUID())) {
			disconnectClient(0x14, "Your character has been namelocked.");
			return false;
		}

		if (gamemasterLogin && player->getAccountType() < ACCOUNT_TYPE_GAMEMASTER) {
			disconnectClient(0x14, "You are not a gamemaster!");
			return false;
		}

		if (g_game.getGameState() == GAME_STATE_CLOSING && !player->hasFlag(PlayerFlag_CanAlwaysLogin)) {
			disconnectClient(0x14, "The game is just going down.\nPlease try again later.");
			return false;
		}

		if (g_game.getGameState() == GAME_STATE_CLOSED && !player->hasFlag(PlayerFlag_CanAlwaysLogin)) {
			disconnectClient(0x14, "Server is currently closed. Please try again later.");
			return false;
		}

		if (g_config.getBoolean(ConfigManager::ONE_PLAYER_ON_ACCOUNT) && player->getAccountType() < ACCOUNT_TYPE_GAMEMASTER && g_game.getPlayerByAccount(player->getAccount())) {
			disconnectClient(0x14, "You may only login with one character\nof your account at the same time.");
			return false;
		}

		if (!player->hasFlag(PlayerFlag_CannotBeBanned)) {
			BanInfo banInfo;
			if (IOBan::getInstance()->isAccountBanned(accountId, banInfo)) {
				if (banInfo.reason.empty()) {
					banInfo.reason = "(none)";
				}

				std::ostringstream ss;
				if (banInfo.expiresAt > 0) {
					ss << "Your account has been banned until " << formatDateShort(banInfo.expiresAt) << " by " << banInfo.bannedBy << ".\n\nReason specified:\n" << banInfo.reason;
				} else {
					ss << "Your account has been permanently banned by " << banInfo.bannedBy << ".\n\nReason specified:\n" << banInfo.reason;
				}
				disconnectClient(0x14, ss.str().c_str());
				return false;
			}
		}

		if (!WaitingList::getInstance()->clientLogin(player)) {
			int32_t currentSlot = WaitingList::getInstance()->getClientSlot(player);
			int32_t retryTime = WaitingList::getTime(currentSlot);
			std::ostringstream ss;

			ss << "Too many players online.\nYou are at place "
			   << currentSlot << " on the waiting list.";

			OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false);
			if (output) {
				output->AddByte(0x16);
				output->AddString(ss.str());
				output->AddByte(retryTime);
				OutputMessagePool::getInstance()->send(output);
			}

			getConnection()->closeConnection();
			return false;
		}

		if (!IOLoginData::getInstance()->loadPlayerByName(player, name)) {
			disconnectClient(0x14, "Your character could not be loaded.");
			return false;
		}

		player->setOperatingSystem((OperatingSystem_t)operatingSystem);

		if (!g_game.placeCreature(player, player->getLoginPosition())) {
			if (!g_game.placeCreature(player, player->getTemplePosition(), false, true)) {
				disconnectClient(0x14, "Temple position is wrong. Contact the administrator.");
				return false;
			}
		}

		player->lastIP = player->getIP();
		player->lastLoginSaved = std::max<time_t>(time(nullptr), player->lastLoginSaved + 1);
		m_acceptPackets = true;
		return true;
	} else {
		if (eventConnect != 0 || !g_config.getBoolean(ConfigManager::REPLACE_KICK_ON_LOGIN)) {
			//Already trying to connect
			disconnectClient(0x14, "You are already logged in.");
			return false;
		}

		if (_player->client) {
			_player->disconnect();
			_player->isConnecting = true;

			addRef();
			eventConnect = g_scheduler.addEvent(createSchedulerTask(1000, boost::bind(&ProtocolGame::connect, this, _player->getID(), operatingSystem)));
			return true;
		}

		addRef();
		return connect(_player->getID(), operatingSystem);
	}
	return false;
}

bool ProtocolGame::connect(uint32_t playerId, OperatingSystem_t operatingSystem)
{
	unRef();
	eventConnect = 0;

	Player* _player = g_game.getPlayerByID(playerId);
	if (!_player || _player->client) {
		disconnectClient(0x14, "You are already logged in.");
		return false;
	}

	player = _player;
	player->useThing2();

	g_chat.removeUserFromAllChannels(*player);
	player->setOperatingSystem((OperatingSystem_t)operatingSystem);
	player->isConnecting = false;

	player->client = this;
	sendAddCreature(player, player->getPosition(), player->getTile()->__getIndexOfThing(player), false);
	player->lastIP = player->getIP();
	player->lastLoginSaved = std::max<time_t>(time(nullptr), player->lastLoginSaved + 1);
	m_acceptPackets = true;
	return true;
}

bool ProtocolGame::logout(bool displayEffect, bool forced)
{
	//dispatcher thread
	if (!player) {
		return false;
	}

	if (!player->isRemoved()) {
		if (!forced && player->getAccountType() != ACCOUNT_TYPE_GOD) {
			if (player->getTile()->hasFlag(TILESTATE_NOLOGOUT)) {
				player->sendCancelMessage(RET_YOUCANNOTLOGOUTHERE);
				return false;
			}

			if (!player->getTile()->hasFlag(TILESTATE_PROTECTIONZONE) && player->hasCondition(CONDITION_INFIGHT)) {
				player->sendCancelMessage(RET_YOUMAYNOTLOGOUTDURINGAFIGHT);
				return false;
			}

			//scripting event - onLogout
			if (!g_creatureEvents->playerLogout(player)) {
				//Let the script handle the error message
				return false;
			}
		}

		if (displayEffect && player->getHealth() > 0) {
			g_game.addMagicEffect(player->getPosition(), NM_ME_POFF);
		}
	}

	if (Connection_ptr connection = getConnection()) {
		connection->closeConnection();
	}
	return g_game.removeCreature(player);
}

bool ProtocolGame::parseFirstPacket(NetworkMessage& msg)
{
	if (g_game.getGameState() == GAME_STATE_SHUTDOWN) {
		getConnection()->closeConnection();
		return false;
	}

	OperatingSystem_t operatingSystem = (OperatingSystem_t)msg.GetU16();
	uint16_t version = msg.GetU16();

	#ifdef __PROTOCOL_77__
	if (!RSA_decrypt(msg)) {
		getConnection()->closeConnection();
		return false;
	}

	uint32_t key[4];
	key[0] = msg.GetU32();
	key[1] = msg.GetU32();
	key[2] = msg.GetU32();
	key[3] = msg.GetU32();
	enableXTEAEncryption();
	setXTEAKey(key);
	#endif

	bool gamemasterFlag = msg.GetByte() != 0;
	uint32_t accountName = msg.GetU32();
	std::string characterName = msg.GetString();
	std::string password = msg.GetString();

	if (version < CLIENT_VERSION_MIN || version > CLIENT_VERSION_MAX) {
		disconnectClient(0x14, "Only clients with protocol " CLIENT_VERSION_STR " allowed!");
		return false;
	}

	if (!accountName) {
		disconnectClient(0x14, "You must enter your account id.");
		return false;
	}

	if (g_game.getGameState() == GAME_STATE_STARTUP || g_game.getServerSaveMessage(0)) {
		disconnectClient(0x14, "Gameworld is starting up. Please wait.");
		return false;
	}

	if (g_game.getGameState() == GAME_STATE_MAINTAIN) {
		disconnectClient(0x14, "Gameworld is under maintenance. Please re-connect in a while.");
		return false;
	}

	BanInfo banInfo;
	if (IOBan::getInstance()->isIpBanned(getIP(), banInfo)) {
		if (banInfo.reason.empty()) {
			banInfo.reason = "(none)";
		}

		std::ostringstream ss;
		ss << "Your IP has been banned until " << formatDateShort(banInfo.expiresAt) << " by " << banInfo.bannedBy << ".\n\nReason specified:\n" << banInfo.reason;
		disconnectClient(0x14, ss.str().c_str());
		return false;
	}

	uint32_t accountId = IOLoginData::getInstance()->gameworldAuthentication(accountName, password, characterName);
	if (accountId == 0) {
		disconnectClient(0x14, "Account id or password is not correct.");
		return false;
	}

	g_dispatcher.addTask(createTask(boost::bind(&ProtocolGame::login, this, characterName, accountId, operatingSystem, gamemasterFlag)));
	return true;
}

void ProtocolGame::onRecvFirstMessage(NetworkMessage& msg)
{
	parseFirstPacket(msg);
}

void ProtocolGame::onConnect()
{
	//
}

void ProtocolGame::disconnectClient(uint8_t error, const char* message)
{
	OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false);
	if (output) {
		output->AddByte(error);
		output->AddString(message);
		OutputMessagePool::getInstance()->send(output);
	}
	disconnect();
}

void ProtocolGame::disconnect()
{
	if (getConnection()) {
		getConnection()->closeConnection();
	}
}

void ProtocolGame::writeToOutputBuffer(const NetworkMessage& msg)
{
	OutputMessage_ptr out = getOutputBuffer(msg.getMessageLength());

	if (out) {
		out->append(msg);
	}
}

void ProtocolGame::parsePacket(NetworkMessage& msg)
{
	if (!m_acceptPackets || g_game.getGameState() == GAME_STATE_SHUTDOWN || msg.getMessageLength() <= 0) {
		return;
	}

	uint8_t recvbyte = msg.GetByte();

	if (!player) {
		if (recvbyte == 0x0F) {
			disconnect();
		}

		return;
	}

	//a dead player can not performs actions
	if (player->isRemoved() || player->getHealth() <= 0) {
		if (recvbyte == 0x0F) {
			disconnect();
			return;
		}

		if (recvbyte != 0x14) {
			return;
		}
	}

	switch (recvbyte) {
		case 0x14: parseLogout(msg); break;
		case 0x1E: parseReceivePing(msg); break;
		case 0x64: parseAutoWalk(msg); break;
		case 0x65: parseMove(msg, NORTH); break;
		case 0x66: parseMove(msg, EAST); break;
		case 0x67: parseMove(msg, SOUTH); break;
		case 0x68: parseMove(msg, WEST); break;
		case 0x69: addGameTask(&Game::playerStopAutoWalk, player->getID()); break;
		case 0x6A: parseMove(msg, NORTHEAST); break;
		case 0x6B: parseMove(msg, SOUTHEAST); break;
		case 0x6C: parseMove(msg, SOUTHWEST); break;
		case 0x6D: parseMove(msg, NORTHWEST); break;
		case 0x6F: parseTurn(msg, NORTH); break;
		case 0x70: parseTurn(msg, EAST); break;
		case 0x71: parseTurn(msg, SOUTH); break;
		case 0x72: parseTurn(msg, WEST); break;
		case 0x78: parseThrow(msg); break;
		case 0x7D: parseRequestTrade(msg); break;
		case 0x7E: parseLookInTrade(msg); break;
		case 0x7F: parseAcceptTrade(msg); break;
		case 0x80: parseCloseTrade(); break;
		case 0x82: parseUseItem(msg); break;
		case 0x83: parseUseItemEx(msg); break;
		case 0x84: parseUseWithCreature(msg); break;
		case 0x85: parseRotateItem(msg); break;
		case 0x87: parseCloseContainer(msg); break;
		case 0x88: parseUpArrowContainer(msg); break;
		case 0x89: parseTextWindow(msg); break;
		case 0x8A: parseHouseWindow(msg); break;
		case 0x8C: parseLookAt(msg); break;
		case 0x8D: parseLookInBattleList(msg); break;
		case 0x96: parseSay(msg); break;
		case 0x97: parseGetChannels(msg); break;
		case 0x98: parseOpenChannel(msg); break;
		case 0x99: parseCloseChannel(msg); break;
		case 0x9A: parseOpenPrivateChannel(msg); break;
		case 0xA0: parseFightModes(msg); break;
		case 0xA1: parseAttack(msg); break;
		case 0xA2: parseFollow(msg); break;
		case 0xA3: parseInviteToParty(msg); break;
		case 0xA4: parseJoinParty(msg); break;
		case 0xA5: parseRevokePartyInvite(msg); break;
		case 0xA6: parsePassPartyLeadership(msg); break;
		case 0xA7: parseLeaveParty(msg); break;
		case 0xAA: parseCreatePrivateChannel(msg); break;
		case 0xAB: parseChannelInvite(msg); break;
		case 0xAC: parseChannelExclude(msg); break;
		case 0xBE: parseCancelMove(msg); break;
		case 0xC9: parseUpdateTile(msg); break;
		case 0xCA: parseUpdateContainer(msg); break;
		case 0xD2: parseRequestOutfit(msg); break;
		case 0xD3: parseSetOutfit(msg); break;
		case 0xDC: parseAddVip(msg); break;
		case 0xDD: parseRemoveVip(msg); break;
		case 0xDE: parseEditVip(msg); break;
		case 0xE6: parseBugReport(msg); break;
		case 0xE8: parseDebugAssert(msg); break;

		default:
			// std::cout << "Player: " << player->getName() << " sent an unknown packet header: 0x" << std::hex << (int16_t)recvbyte << std::dec << "!" << std::endl;
			break;
	}

	if (msg.isOverrun()) {
		disconnect();
	}
}

void ProtocolGame::GetTileDescription(const Tile* tile, NetworkMessage& msg)
{
	int32_t count;
	if (tile->ground) {
		msg.AddItem(tile->ground);
		count = 1;
	} else {
		count = 0;
	}

	const TileItemVector* items = tile->getItemList();
	if (items) {
		for (auto it = items->getBeginTopItem(); it != items->getEndTopItem(); ++it) {
			msg.AddItem(*it);

			if(++count == 10) {
				return;
			}
		}
	}

	const CreatureVector* creatures = tile->getCreatures();
	if (creatures) {
		for (auto it = creatures->begin(); it != creatures->end(); ++it) {
			if (!player->canSeeCreature(*it)) {
				continue;
			}

			bool known;
			uint32_t removedKnown;
			checkCreatureAsKnown((*it)->getID(), known, removedKnown);
			AddCreature(msg, *it, known, removedKnown);

			if(++count == 10) {
				return;
			}
		}
	}

	if (items) {
		for (auto it = items->getBeginDownItem(); it != items->getEndDownItem(); ++it) {
			msg.AddItem(*it);

			if(++count == 10) {
				return;
			}
		}
	}
}

void ProtocolGame::GetMapDescription(int32_t x, int32_t y, int32_t z, int32_t width, int32_t height, NetworkMessage& msg)
{
	int32_t skip = -1;
	int32_t startz, endz, zstep = 0;

	if (z > 7) {
		startz = z - 2;
		endz = std::min<int32_t>(MAP_MAX_LAYERS - 1, z + 2);
		zstep = 1;
	} else {
		startz = 7;
		endz = 0;
		zstep = -1;
	}

	for (int32_t nz = startz; nz != endz + zstep; nz += zstep) {
		GetFloorDescription(msg, x, y, nz, width, height, z - nz, skip);
	}

	if (skip >= 0) {
		msg.AddByte(skip);
		msg.AddByte(0xFF);
	}
}

void ProtocolGame::GetFloorDescription(NetworkMessage& msg, int32_t x, int32_t y, int32_t z, int32_t width, int32_t height, int32_t offset, int32_t& skip)
{
	for (int32_t nx = 0; nx < width; nx++) {
		for (int32_t ny = 0; ny < height; ny++) {
			Tile* tile = g_game.getTile(x + nx + offset, y + ny + offset, z);
			if (tile) {
				if (skip >= 0) {
					msg.AddByte(skip);
					msg.AddByte(0xFF);
				}

				skip = 0;
				GetTileDescription(tile, msg);
			} else if (skip == 0xFE) {
				msg.AddByte(0xFF);
				msg.AddByte(0xFF);
				skip = -1;
			} else {
				++skip;
			}
		}
	}
}

void ProtocolGame::checkCreatureAsKnown(uint32_t id, bool& known, uint32_t& removedKnown)
{
	auto result = knownCreatureSet.insert(id);
	if (!result.second) {
		known = true;
		return;
	}

	known = false;

	if (knownCreatureSet.size() > 150) {
		// Look for a creature to remove
		for (std::unordered_set<uint32_t>::iterator it = knownCreatureSet.begin(); it != knownCreatureSet.end(); ++it) {
			Creature* creature = g_game.getCreatureByID(*it);
			if (!canSee(creature)) {
				removedKnown = *it;
				knownCreatureSet.erase(it);
				return;
			}
		}

		// Bad situation. Let's just remove anyone.
		std::unordered_set<uint32_t>::iterator it = knownCreatureSet.begin();
		if (*it == id) {
			++it;
		}

		removedKnown = *it;
		knownCreatureSet.erase(it);
	} else {
		removedKnown = 0;
	}
}

bool ProtocolGame::canSee(const Creature* c) const
{
	if (!c || !player || c->isRemoved()) {
		return false;
	}

	if (!player->canSeeCreature(c)) {
		return false;
	}

	return canSee(c->getPosition());
}

bool ProtocolGame::canSee(const Position& pos) const
{
	return canSee(pos.x, pos.y, pos.z);
}

bool ProtocolGame::canSee(int32_t x, int32_t y, int32_t z) const
{
	if (!player) {
		return false;
	}

	const Position& myPos = player->getPosition();
	if (myPos.z <= 7) {
		//we are on ground level or above (7 -> 0)
		//view is from 7 -> 0
		if (z > 7) {
			return false;
		}
	} else if (myPos.z >= 8) {
		//we are underground (8 -> 15)
		//view is +/- 2 from the floor we stand on
		if (std::abs(myPos.getZ() - z) > 2) {
			return false;
		}
	}

	//negative offset means that the action taken place is on a lower floor than ourself
	int32_t offsetz = myPos.getZ() - z;
	if ((x >= myPos.getX() - 8 + offsetz) && (x <= myPos.getX() + 9 + offsetz) &&
	        (y >= myPos.getY() - 6 + offsetz) && (y <= myPos.getY() + 7 + offsetz)) {
		return true;
	}
	return false;
}

//********************** Parse methods *******************************//
void ProtocolGame::parseLogout(NetworkMessage& msg)
{
	g_dispatcher.addTask(createTask(boost::bind(&ProtocolGame::logout, this, true, false)));
}

void ProtocolGame::parseCreatePrivateChannel(NetworkMessage& msg)
{
	addGameTask(&Game::playerCreatePrivateChannel, player->getID());
}

void ProtocolGame::parseChannelInvite(NetworkMessage& msg)
{
	const std::string name = msg.GetString();
	addGameTask(&Game::playerChannelInvite, player->getID(), name);
}

void ProtocolGame::parseChannelExclude(NetworkMessage& msg)
{
	const std::string name = msg.GetString();
	addGameTask(&Game::playerChannelExclude, player->getID(), name);
}

void ProtocolGame::parseGetChannels(NetworkMessage& msg)
{
	addGameTask(&Game::playerRequestChannels, player->getID());
}

void ProtocolGame::parseOpenChannel(NetworkMessage& msg)
{
	uint16_t channelId = msg.GetU16();
	addGameTask(&Game::playerOpenChannel, player->getID(), channelId);
}

void ProtocolGame::parseCloseChannel(NetworkMessage& msg)
{
	uint16_t channelId = msg.GetU16();
	addGameTask(&Game::playerCloseChannel, player->getID(), channelId);
}

void ProtocolGame::parseOpenPrivateChannel(NetworkMessage& msg)
{
	const std::string receiver = msg.GetString();
	addGameTask(&Game::playerOpenPrivateChannel, player->getID(), receiver);
}

void ProtocolGame::parseCancelMove(NetworkMessage& msg)
{
	addGameTask(&Game::playerCancelAttackAndFollow, player->getID());
}

void ProtocolGame::parseReceivePing(NetworkMessage& msg)
{
	addGameTask(&Game::playerReceivePing, player->getID());
}

void ProtocolGame::parseAutoWalk(NetworkMessage& msg)
{
	std::list<Direction> path;

	size_t numdirs = msg.GetByte();
	for (size_t i = 0; i < numdirs; ++i) {
		uint8_t rawdir = msg.GetByte();
		switch (rawdir) {
			case 1: path.push_back(EAST); break;
			case 2: path.push_back(NORTHEAST); break;
			case 3: path.push_back(NORTH); break;
			case 4: path.push_back(NORTHWEST); break;
			case 5: path.push_back(WEST); break;
			case 6: path.push_back(SOUTHWEST); break;
			case 7: path.push_back(SOUTH); break;
			case 8: path.push_back(SOUTHEAST); break;
			default: break;
		}
	}

	if (path.empty()) {
		return;
	}

	addGameTask(&Game::playerAutoWalk, player->getID(), path);
}

void ProtocolGame::parseMove(NetworkMessage& msg, Direction dir)
{
	addGameTask(&Game::playerMove, player->getID(), dir);
}

void ProtocolGame::parseTurn(NetworkMessage& msg, Direction dir)
{
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerTurn, player->getID(), dir);
}

void ProtocolGame::parseRequestOutfit(NetworkMessage& msg)
{
	addGameTask(&Game::playerRequestOutfit, player->getID());
}

void ProtocolGame::parseSetOutfit(NetworkMessage& msg)
{
	Outfit_t newOutfit;
	#ifdef __PROTOCOL_77__
	newOutfit.lookType = msg.GetU16();
	#else
	newOutfit.lookType = msg.GetByte();
	#endif
	newOutfit.lookHead = msg.GetByte();
	newOutfit.lookBody = msg.GetByte();
	newOutfit.lookLegs = msg.GetByte();
	newOutfit.lookFeet = msg.GetByte();
	addGameTask(&Game::playerChangeOutfit, player->getID(), newOutfit);
}

void ProtocolGame::parseUseItem(NetworkMessage& msg)
{
	Position pos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	uint8_t stackpos = msg.GetByte();
	uint8_t index = msg.GetByte();
	bool isHotkey = (pos.x == 0xFFFF && pos.y == 0 && pos.z == 0);
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerUseItem, player->getID(), pos, stackpos, index, spriteId, isHotkey);
}

void ProtocolGame::parseUseItemEx(NetworkMessage& msg)
{
	Position fromPos = msg.GetPosition();
	uint16_t fromSpriteId = msg.GetSpriteId();
	uint8_t fromStackPos = msg.GetByte();
	Position toPos = msg.GetPosition();
	uint16_t toSpriteId = msg.GetU16();
	uint8_t toStackPos = msg.GetByte();
	bool isHotkey = (fromPos.x == 0xFFFF && fromPos.y == 0 && fromPos.z == 0);
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerUseItemEx, player->getID(), fromPos, fromStackPos, fromSpriteId, toPos, toStackPos, toSpriteId, isHotkey);
}

void ProtocolGame::parseUseWithCreature(NetworkMessage& msg)
{
	Position fromPos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	uint8_t fromStackPos = msg.GetByte();
	uint32_t creatureId = msg.GetU32();
	bool isHotkey = (fromPos.x == 0xFFFF && fromPos.y == 0 && fromPos.z == 0);
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerUseWithCreature, player->getID(), fromPos, fromStackPos, creatureId, spriteId, isHotkey);
}

void ProtocolGame::parseCloseContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.GetByte();
	addGameTask(&Game::playerCloseContainer, player->getID(), cid);
}

void ProtocolGame::parseUpArrowContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.GetByte();
	addGameTask(&Game::playerMoveUpContainer, player->getID(), cid);
}

void ProtocolGame::parseUpdateTile(NetworkMessage& msg)
{
	// Position pos = msg.GetPosition();
	// addGameTask(&Game::playerUpdateTile, player->getID(), pos);
}

void ProtocolGame::parseUpdateContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.GetByte();
	addGameTask(&Game::playerUpdateContainer, player->getID(), cid);
}

void ProtocolGame::parseThrow(NetworkMessage& msg)
{
	Position fromPos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	uint8_t fromStackpos = msg.GetByte();
	Position toPos = msg.GetPosition();
	uint8_t count = msg.GetByte();

	if (toPos != fromPos) {
		addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerMoveThing, player->getID(), fromPos, spriteId, fromStackpos, toPos, count);
	}
}

void ProtocolGame::parseLookAt(NetworkMessage& msg)
{
	Position pos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	uint8_t stackpos = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerLookAt, player->getID(), pos, spriteId, stackpos);
}

void ProtocolGame::parseLookInBattleList(NetworkMessage& msg)
{
	uint32_t creatureId = msg.GetU32();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerLookInBattleList, player->getID(), creatureId);
}

void ProtocolGame::parseSay(NetworkMessage& msg)
{
	SpeakClasses type = (SpeakClasses)msg.GetByte();

	std::string receiver;
	uint16_t channelId = 0;

	switch (type) {
		case SPEAK_PRIVATE:
		case SPEAK_PRIVATE_RED:
			receiver = msg.GetString();
			break;

		case SPEAK_CHANNEL_Y:
		case SPEAK_CHANNEL_R1:
			channelId = msg.GetU16();
			break;

		default:
			break;
	}

	const std::string text = msg.GetString();

	if (text.length() > 255) {
		return;
	}

	addGameTask(&Game::playerSay, player->getID(), channelId, type, receiver, text);
}

void ProtocolGame::parseFightModes(NetworkMessage& msg)
{
	uint8_t rawFightMode = msg.GetByte(); //1 - offensive, 2 - balanced, 3 - defensive
	uint8_t rawChaseMode = msg.GetByte(); // 0 - stand while fightning, 1 - chase opponent
	uint8_t rawSecureMode = msg.GetByte(); // 0 - can't attack unmarked, 1 - can attack unmarked

	chaseMode_t chaseMode;

	if (rawChaseMode == 1) {
		chaseMode = CHASEMODE_FOLLOW;
	} else {
		chaseMode = CHASEMODE_STANDSTILL;
	}

	fightMode_t fightMode;

	if (rawFightMode == 1) {
		fightMode = FIGHTMODE_ATTACK;
	} else if (rawFightMode == 2) {
		fightMode = FIGHTMODE_BALANCED;
	} else {
		fightMode = FIGHTMODE_DEFENSE;
	}

	secureMode_t secureMode;

	if (rawSecureMode == 1) {
		secureMode = SECUREMODE_ON;
	} else {
		secureMode = SECUREMODE_OFF;
	}

	addGameTask(&Game::playerSetFightModes, player->getID(), fightMode, chaseMode, secureMode);
}

void ProtocolGame::parseAttack(NetworkMessage& msg)
{
	uint32_t creatureId = msg.GetU32();
	addGameTask(&Game::playerSetAttackedCreature, player->getID(), creatureId);
}

void ProtocolGame::parseFollow(NetworkMessage& msg)
{
	uint32_t creatureId = msg.GetU32();
	addGameTask(&Game::playerFollowCreature, player->getID(), creatureId);
}

void ProtocolGame::parseTextWindow(NetworkMessage& msg)
{
	uint32_t windowTextId = msg.GetU32();
	const std::string newText = msg.GetString();
	addGameTask(&Game::playerWriteItem, player->getID(), windowTextId, newText);
}

void ProtocolGame::parseHouseWindow(NetworkMessage& msg)
{
	uint8_t doorId = msg.GetByte();
	uint32_t id = msg.GetU32();
	const std::string text = msg.GetString();
	addGameTask(&Game::playerUpdateHouseWindow, player->getID(), doorId, id, text);
}

void ProtocolGame::parseRequestTrade(NetworkMessage& msg)
{
	Position pos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	uint8_t stackpos = msg.GetByte();
	uint32_t playerId = msg.GetU32();
	addGameTask(&Game::playerRequestTrade, player->getID(), pos, stackpos, playerId, spriteId);
}

void ProtocolGame::parseAcceptTrade(NetworkMessage& msg)
{
	addGameTask(&Game::playerAcceptTrade, player->getID());
}

void ProtocolGame::parseLookInTrade(NetworkMessage& msg)
{
	bool counterOffer = (msg.GetByte() == 0x01);
	uint8_t index = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerLookInTrade, player->getID(), counterOffer, index);
}

void ProtocolGame::parseCloseTrade()
{
	addGameTask(&Game::playerCloseTrade, player->getID());
}

void ProtocolGame::parseAddVip(NetworkMessage& msg)
{
	const std::string name = msg.GetString();
	addGameTask(&Game::playerRequestAddVip, player->getID(), name);
}

void ProtocolGame::parseRemoveVip(NetworkMessage& msg)
{
	uint32_t guid = msg.GetU32();
	addGameTask(&Game::playerRequestRemoveVip, player->getID(), guid);
}

void ProtocolGame::parseEditVip(NetworkMessage& msg)
{
	uint32_t guid = msg.GetU32();
	const std::string description = msg.GetString();
	uint32_t icon = std::min<uint32_t>(10, msg.GetU32()); // 10 is max icon in 9.63
	bool notify = msg.GetByte() != 0;
	addGameTask(&Game::playerRequestEditVip, player->getID(), guid, description, icon, notify);
}

void ProtocolGame::parseRotateItem(NetworkMessage& msg)
{
	Position pos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	uint8_t stackpos = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerRotateItem, player->getID(), pos, stackpos, spriteId);
}

void ProtocolGame::parseBugReport(NetworkMessage& msg)
{
	std::string bug = msg.GetString();
	addGameTask(&Game::playerReportBug, player->getID(), bug);
}

void ProtocolGame::parseDebugAssert(NetworkMessage& msg)
{
	if (m_debugAssertSent) {
		return;
	}

	m_debugAssertSent = true;

	std::string assertLine = msg.GetString();
	std::string date = msg.GetString();
	std::string description = msg.GetString();
	std::string comment = msg.GetString();
	addGameTask(&Game::playerDebugAssert, player->getID(), assertLine, date, description, comment);
}

void ProtocolGame::parseInviteToParty(NetworkMessage& msg)
{
	uint32_t targetId = msg.GetU32();
	addGameTask(&Game::playerInviteToParty, player->getID(), targetId);
}

void ProtocolGame::parseJoinParty(NetworkMessage& msg)
{
	uint32_t targetId = msg.GetU32();
	addGameTask(&Game::playerJoinParty, player->getID(), targetId);
}

void ProtocolGame::parseRevokePartyInvite(NetworkMessage& msg)
{
	uint32_t targetId = msg.GetU32();
	addGameTask(&Game::playerRevokePartyInvitation, player->getID(), targetId);
}

void ProtocolGame::parsePassPartyLeadership(NetworkMessage& msg)
{
	uint32_t targetId = msg.GetU32();
	addGameTask(&Game::playerPassPartyLeadership, player->getID(), targetId);
}

void ProtocolGame::parseLeaveParty(NetworkMessage& msg)
{
	addGameTask(&Game::playerLeaveParty, player->getID());
}

//********************** Send methods *******************************//
void ProtocolGame::sendOpenPrivateChannel(const std::string& receiver)
{
	NetworkMessage msg;
	msg.AddByte(0xAD);
	msg.AddString(receiver);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureOutfit(const Creature* creature, const Outfit_t& outfit)
{
	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	msg.AddByte(0x8E);
	msg.AddU32(creature->getID());
	AddCreatureOutfit(msg, creature, outfit);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureLight(const Creature* creature)
{
	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	AddCreatureLight(msg, creature);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendWorldLight(const LightInfo& lightInfo)
{
	NetworkMessage msg;
	AddWorldLight(msg, lightInfo);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureShield(const Creature* creature)
{
	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	msg.AddByte(0x91);
	msg.AddU32(creature->getID());
	msg.AddByte(player->getPartyShield(creature->getPlayer()));
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureSkull(const Creature* creature)
{
	if (g_game.getWorldType() != WORLD_TYPE_PVP) {
		return;
	}

	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	msg.AddByte(0x90);
	msg.AddU32(creature->getID());
	msg.AddByte(player->getSkullClient(creature->getPlayer()));
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureSquare(const Creature* creature, SquareColor_t color)
{
	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	msg.AddByte(0x86);
	msg.AddU32(creature->getID());
	msg.AddByte((uint8_t)color);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendStats()
{
	NetworkMessage msg;
	AddPlayerStats(msg);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendTextMessage(MessageClasses mclass, const std::string& message)
{
	NetworkMessage msg;
	AddTextMessage(msg, mclass, message);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendClosePrivate(uint16_t channelId)
{
	NetworkMessage msg;
	msg.AddByte(0xB3);
	msg.AddU16(channelId);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatePrivateChannel(uint16_t channelId, const std::string& channelName)
{
	NetworkMessage msg;
	msg.AddByte(0xB2);
	msg.AddU16(channelId);
	msg.AddString(channelName);
	msg.AddU16(0x01);
	msg.AddString(player->getName());
	msg.AddU16(0x00);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendChannelsDialog()
{
	NetworkMessage msg;
	msg.AddByte(0xAB);

	const ChannelList& list = g_chat.getChannelList(*player);
	msg.AddByte(list.size());
	for (ChatChannel* channel : list) {
		msg.AddU16(channel->getId());
		msg.AddString(channel->getName());
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendChannel(uint16_t channelId, const std::string& channelName, const UsersMap* channelUsers, const InvitedMap* invitedUsers)
{
	NetworkMessage msg;
	msg.AddByte(0xAC);

	msg.AddU16(channelId);
	msg.AddString(channelName);

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendChannelMessage(const std::string& author, const std::string& text, SpeakClasses type, uint16_t channel)
{
	NetworkMessage msg;
	msg.AddByte(0xAA);
	#ifdef __PROTOCOL_77__
	msg.AddU32(0x00);
	#endif
	msg.AddString(author);
	msg.AddByte(type);
	msg.AddU16(channel);
	msg.AddString(text);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendIcons(uint16_t icons)
{
	NetworkMessage msg;
	msg.AddByte(0xA2);
	msg.AddByte(icons);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendContainer(uint8_t cid, const Container* container, bool hasParent, uint16_t firstIndex)
{
	NetworkMessage msg;
	msg.AddByte(0x6E);

	msg.AddByte(cid);
	msg.AddItem(container);
	msg.AddString(container->getName());
	msg.AddByte(container->capacity());
	msg.AddByte(hasParent ? 0x01 : 0x00);
	msg.AddByte(std::min<uint32_t>(0xFF, container->size()));
	uint32_t i = 0;
	const ItemDeque& itemList = container->getItemList();
	for (ItemDeque::const_iterator it = itemList.begin(), end = itemList.end(); i < 0xFF && it != end; ++it, ++i) {
		msg.AddItem(*it);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendTradeItemRequest(const Player* player, const Item* item, bool ack)
{
	NetworkMessage msg;

	if (ack) {
		msg.AddByte(0x7D);
	} else {
		msg.AddByte(0x7E);
	}

	msg.AddString(player->getName());

	if (const Container* tradeContainer = item->getContainer()) {
		std::list<const Container*> listContainer;
		listContainer.push_back(tradeContainer);

		std::list<const Item*> itemList;
		itemList.push_back(tradeContainer);

		while (!listContainer.empty()) {
			const Container* container = listContainer.front();
			listContainer.pop_front();

			for (Item* containerItem : container->getItemList()) {
				Container* tmpContainer = containerItem->getContainer();
				if (tmpContainer) {
					listContainer.push_back(tmpContainer);
				}
				itemList.push_back(containerItem);
			}
		}

		msg.AddByte(itemList.size());
		for (const Item* listItem : itemList) {
			msg.AddItem(listItem);
		}
	} else {
		msg.AddByte(0x01);
		msg.AddItem(item);
	}
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCloseTrade()
{
	NetworkMessage msg;
	msg.AddByte(0x7F);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCloseContainer(uint8_t cid)
{
	NetworkMessage msg;
	msg.AddByte(0x6F);
	msg.AddByte(cid);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureTurn(const Creature* creature, uint32_t stackPos)
{
	if (stackPos >= 10 || !canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	msg.AddByte(0x6B);
	msg.AddPosition(creature->getPosition());
	msg.AddByte(stackPos);
	msg.AddU16(0x63);
	msg.AddU32(creature->getID());
	msg.AddByte(creature->getDirection());
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureSay(const Creature* creature, SpeakClasses type, const std::string& text, Position* pos/* = nullptr*/)
{
	NetworkMessage msg;
	AddCreatureSpeak(msg, creature, type, text, 0, pos);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendToChannel(const Creature* creature, SpeakClasses type, const std::string& text, uint16_t channelId)
{
	NetworkMessage msg;
	AddCreatureSpeak(msg, creature, type, text, channelId);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCancelTarget()
{
	NetworkMessage msg;
	msg.AddByte(0xA3);
	msg.AddU32(0x00);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendChangeSpeed(const Creature* creature, uint32_t speed)
{
	NetworkMessage msg;
	msg.AddByte(0x8F);
	msg.AddU32(creature->getID());
	msg.AddU16(speed);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCancelWalk()
{
	NetworkMessage msg;
	msg.AddByte(0xB5);
	msg.AddByte(player->getDirection());
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendSkills()
{
	NetworkMessage msg;
	AddPlayerSkills(msg);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendPing()
{
	NetworkMessage msg;
	msg.AddByte(0x1E);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendDistanceShoot(const Position& from, const Position& to, uint8_t type)
{
	NetworkMessage msg;
	AddDistanceShoot(msg, from, to, type);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendAnimatedText(const Position& pos, uint8_t color, const std::string& text)
{
	if(!canSee(pos)) {
		return;
	}

	NetworkMessage msg;
	AddAnimatedText(msg, pos, color, text);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendMagicEffect(const Position& pos, uint8_t type)
{
	if (!canSee(pos)) {
		return;
	}

	NetworkMessage msg;
	AddMagicEffect(msg, pos, type);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureHealth(const Creature* creature)
{
	NetworkMessage msg;
	AddCreatureHealth(msg, creature);
	writeToOutputBuffer(msg);
}

//tile
void ProtocolGame::sendMapDescription(const Position& pos)
{
	NetworkMessage msg;
	msg.AddByte(0x64);
	msg.AddPosition(player->getPosition());
	GetMapDescription(pos.x - 8, pos.y - 6, pos.z, 18, 14, msg);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendAddTileItem(const Tile* tile, const Position& pos, uint32_t stackpos, const Item* item)
{
	if (!canSee(pos)) {
		return;
	}

	NetworkMessage msg;
	AddTileItem(msg, pos, stackpos, item);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendUpdateTileItem(const Tile* tile, const Position& pos, uint32_t stackpos, const Item* item)
{
	if (!canSee(pos)) {
		return;
	}

	NetworkMessage msg;
	UpdateTileItem(msg, pos, stackpos, item);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendRemoveTileItem(const Tile* tile, const Position& pos, uint32_t stackpos)
{
	if (!canSee(pos)) {
		return;
	}

	NetworkMessage msg;
	RemoveTileItem(msg, pos, stackpos);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendUpdateTile(const Tile* tile, const Position& pos)
{
	if (!canSee(pos)) {
		return;
	}

	NetworkMessage msg;
	msg.AddByte(0x69);
	msg.AddPosition(pos);

	if (tile) {
		GetTileDescription(tile, msg);
		msg.AddByte(0x00);
		msg.AddByte(0xFF);
	} else {
		msg.AddByte(0x01);
		msg.AddByte(0xFF);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendAddCreature(const Creature* creature, const Position& pos, uint32_t stackpos, bool isLogin)
{
	if (!canSee(pos)) {
		return;
	}

	NetworkMessage msg;

	if (creature != player) {
		AddTileCreature(msg, pos, stackpos, creature);
		writeToOutputBuffer(msg);

		if (isLogin) {
			sendMagicEffect(pos, NM_ME_TELEPORT);
		}

		return;
	}

	msg.AddByte(0x0A);
	msg.AddU32(player->getID());
	msg.AddByte(0x32); // beat duration (50)
	msg.AddByte(0x00);
	// can report bugs?
	if (player->getAccountType() >= ACCOUNT_TYPE_TUTOR) {
		msg.AddByte(0x01);
	} else {
		msg.AddByte(0x00);
	}

	if(violationReasons[player->getAccountType()] > 0)
	{
		msg.AddByte(0x0B);
		for(int32_t i = 0; i < 32; i++)
		{
			if(i < violationReasons[player->getAccountType()])
				msg.AddByte(violationActions[player->getAccountType()]);
			else
				msg.AddByte(0x00);
		}
	}

	writeToOutputBuffer(msg);

	sendMapDescription(pos);

	if (isLogin) {
		sendMagicEffect(pos, NM_ME_TELEPORT);
	}

	sendInventoryItem(SLOT_HEAD, player->getInventoryItem(SLOT_HEAD));
	sendInventoryItem(SLOT_NECKLACE, player->getInventoryItem(SLOT_NECKLACE));
	sendInventoryItem(SLOT_BACKPACK, player->getInventoryItem(SLOT_BACKPACK));
	sendInventoryItem(SLOT_ARMOR, player->getInventoryItem(SLOT_ARMOR));
	sendInventoryItem(SLOT_RIGHT, player->getInventoryItem(SLOT_RIGHT));
	sendInventoryItem(SLOT_LEFT, player->getInventoryItem(SLOT_LEFT));
	sendInventoryItem(SLOT_LEGS, player->getInventoryItem(SLOT_LEGS));
	sendInventoryItem(SLOT_FEET, player->getInventoryItem(SLOT_FEET));
	sendInventoryItem(SLOT_RING, player->getInventoryItem(SLOT_RING));
	sendInventoryItem(SLOT_AMMO, player->getInventoryItem(SLOT_AMMO));

	sendStats();
	sendSkills();

	//gameworld light-settings
	LightInfo lightInfo;
	g_game.getWorldLightInfo(lightInfo);
	sendWorldLight(lightInfo);

	//player light level
	sendCreatureLight(creature);

	const std::list<VIPEntry>& vipEntries = IOLoginData::getInstance()->getVIPEntries(player->getAccount());

	if (player->isAccessPlayer()) {
		for (const VIPEntry& entry : vipEntries) {
			VipStatus_t vipStatus;

			Player* vipPlayer = g_game.getPlayerByGUID(entry.guid);
			if (!vipPlayer) {
				vipStatus = VIPSTATUS_OFFLINE;
			} else {
				vipStatus = VIPSTATUS_ONLINE;
			}

			sendVIP(entry.guid, entry.name, entry.description, entry.icon, entry.notify, vipStatus);
		}
	} else {
		for (const VIPEntry& entry : vipEntries) {
			VipStatus_t vipStatus;

			Player* vipPlayer = g_game.getPlayerByGUID(entry.guid);
			if (!vipPlayer || vipPlayer->isInGhostMode()) {
				vipStatus = VIPSTATUS_OFFLINE;
			} else {
				vipStatus = VIPSTATUS_ONLINE;
			}

			sendVIP(entry.guid, entry.name, entry.description, entry.icon, entry.notify, vipStatus);
		}
	}

	player->sendIcons();
}

void ProtocolGame::sendRemoveCreature(const Creature* creature, const Position& pos, uint32_t stackpos)
{
	if (!canSee(pos)) {
		return;
	}

	NetworkMessage msg;
	RemoveTileItem(msg, pos, stackpos);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendMoveCreature(const Creature* creature, const Tile* newTile, const Position& newPos, uint32_t newStackPos, const Tile* oldTile, const Position& oldPos, uint32_t oldStackPos, bool teleport)
{
	if (creature == player) {
		if (teleport || oldStackPos >= 10) {
			NetworkMessage msg;
			RemoveTileItem(msg, oldPos, oldStackPos);
			writeToOutputBuffer(msg);
			sendMapDescription(newPos);
		} else {
			NetworkMessage msg;

			if (oldPos.z == 7 && newPos.z >= 8) {
				RemoveTileItem(msg, oldPos, oldStackPos);
			} else {
				msg.AddByte(0x6D);
				msg.AddPosition(oldPos);
				msg.AddByte(oldStackPos);
				msg.AddPosition(newPos);
			}

			if (newPos.z > oldPos.z) {
				MoveDownCreature(msg, creature, newPos, oldPos, oldStackPos);
			} else if (newPos.z < oldPos.z) {
				MoveUpCreature(msg, creature, newPos, oldPos, oldStackPos);
			}

			if (oldPos.y > newPos.y) { // north, for old x
				msg.AddByte(0x65);
				GetMapDescription(oldPos.x - 8, newPos.y - 6, newPos.z, 18, 1, msg);
			} else if (oldPos.y < newPos.y) { // south, for old x
				msg.AddByte(0x67);
				GetMapDescription(oldPos.x - 8, newPos.y + 7, newPos.z, 18, 1, msg);
			}

			if (oldPos.x < newPos.x) { // east, [with new y]
				msg.AddByte(0x66);
				GetMapDescription(newPos.x + 9, newPos.y - 6, newPos.z, 1, 14, msg);
			} else if (oldPos.x > newPos.x) { // west, [with new y]
				msg.AddByte(0x68);
				GetMapDescription(newPos.x - 8, newPos.y - 6, newPos.z, 1, 14, msg);
			}

			writeToOutputBuffer(msg);
		}
	} else if (canSee(oldPos) && canSee(creature->getPosition())) {
		if (teleport || (oldPos.z == 7 && newPos.z >= 8) || oldStackPos >= 10) {
			sendRemoveCreature(creature, oldPos, oldStackPos);
			sendAddCreature(creature, newPos, newStackPos, false);
		} else {
			NetworkMessage msg;
			msg.AddByte(0x6D);
			msg.AddPosition(oldPos);
			msg.AddByte(oldStackPos);
			msg.AddPosition(creature->getPosition());
			writeToOutputBuffer(msg);
		}
	} else if (canSee(oldPos)) {
		sendRemoveCreature(creature, oldPos, oldStackPos);
	} else if (canSee(creature->getPosition())) {
		sendAddCreature(creature, newPos, newStackPos, false);
	}
}

void ProtocolGame::sendInventoryItem(slots_t slot, const Item* item)
{
	NetworkMessage msg;

	if (item) {
		msg.AddByte(0x78);
		msg.AddByte(slot);
		msg.AddItem(item);
	} else {
		msg.AddByte(0x79);
		msg.AddByte(slot);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendAddContainerItem(uint8_t cid, uint16_t slot, const Item* item)
{
	NetworkMessage msg;
	msg.AddByte(0x70);
	msg.AddByte(cid);
	msg.AddItem(item);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendUpdateContainerItem(uint8_t cid, uint16_t slot, const Item* item)
{
	NetworkMessage msg;
	msg.AddByte(0x71);
	msg.AddByte(cid);
	msg.AddByte(slot);
	msg.AddItem(item);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendRemoveContainerItem(uint8_t cid, uint16_t slot, const Item* lastItem)
{
	NetworkMessage msg;
	msg.AddByte(0x72);
	msg.AddByte(cid);
	msg.AddByte(slot);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendTextWindow(uint32_t windowTextId, Item* item, uint16_t maxlen, bool canWrite)
{
	NetworkMessage msg;
	msg.AddByte(0x96);
	msg.AddU32(windowTextId);
	msg.AddItem(item);

	if (canWrite) {
		msg.AddU16(maxlen);
		msg.AddString(item->getText());
	} else {
		const std::string& text = item->getText();
		msg.AddU16(text.size());
		msg.AddString(text);
	}

	#ifdef __PROTOCOL_76__
	const std::string& writer = item->getWriter();
	if (writer.size()) {
		msg.AddString(writer);
	} else {
		msg.AddString("");
	}
	#endif
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendTextWindow(uint32_t windowTextId, uint32_t itemId, const std::string& text)
{
	NetworkMessage msg;
	msg.AddByte(0x96);
	msg.AddU32(windowTextId);
	msg.AddItem(itemId, 1);
	msg.AddU16(text.size());
	msg.AddString(text);
	msg.AddString("");
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendHouseWindow(uint32_t windowTextId, House* _house, uint32_t listId, const std::string& text)
{
	NetworkMessage msg;
	msg.AddByte(0x97);
	msg.AddByte(0x00);
	msg.AddU32(windowTextId);
	msg.AddString(text);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendOutfitWindow()
{
	NetworkMessage msg;
	msg.AddByte(0xC8);
	AddCreatureOutfit(msg, player, player->getCurrentOutfit());
	#ifdef __PROTOCOL_77__
	msg.AddU16(player->sex % 2 ? 128 : 136);
	msg.AddU16(player->isPremium() ? (player->sex % 2 ? 134 : 142) : (player->sex % 2 ? 131 : 139));
	#else
	msg.AddByte(player->sex % 2 ? 128 : 136);
	msg.AddByte(player->isPremium() ? (player->sex % 2 ? 134 : 142) : (player->sex % 2 ? 131 : 139));
	#endif
	player->hasRequestedOutfit(true);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendVIPLogIn(uint32_t guid)
{
	NetworkMessage msg;
	msg.AddByte(0xD3);
	msg.AddU32(guid);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendVIPLogOut(uint32_t guid)
{
	NetworkMessage msg;
	msg.AddByte(0xD4);
	msg.AddU32(guid);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendVIP(uint32_t guid, const std::string& name, const std::string& description, uint32_t icon, bool notify, VipStatus_t status)
{
	NetworkMessage msg;
	msg.AddByte(0xD2);
	msg.AddU32(guid);
	msg.AddString(name);
	msg.AddByte(notify ? 0x01 : 0x00);
	writeToOutputBuffer(msg);
}

////////////// Add common messages
void ProtocolGame::AddTextMessage(NetworkMessage& msg, MessageClasses mclass, const std::string& message)
{
	msg.AddByte(0xB4);
	msg.AddByte(mclass);
	msg.AddString(message);
}

void ProtocolGame::AddAnimatedText(NetworkMessage& msg, const Position& pos, uint8_t color, const std::string& text)
{
	msg.AddByte(0x84);
	msg.AddPosition(pos);
	msg.AddByte(color);
	msg.AddString(text);
}

void ProtocolGame::AddMagicEffect(NetworkMessage& msg, const Position& pos, uint8_t type)
{
	msg.AddByte(0x83);
	msg.AddPosition(pos);
	#ifdef __PROTOCOL_76__
	msg.AddByte(type + 1);
	#else
	msg.AddByte(type);
	#endif
}

void ProtocolGame::AddDistanceShoot(NetworkMessage& msg, const Position& from, const Position& to, uint8_t type)
{
	msg.AddByte(0x85);
	msg.AddPosition(from);
	msg.AddPosition(to);
	#ifdef __PROTOCOL_76__
	msg.AddByte(type + 1);
	#else
	msg.AddByte(type);
	#endif
}

void ProtocolGame::AddCreature(NetworkMessage& msg, const Creature* creature, bool known, uint32_t remove)
{
	const Player* otherPlayer = creature->getPlayer();

	if (known) {
		msg.AddU16(0x62);
		msg.AddU32(creature->getID());
	} else {
		msg.AddU16(0x61);
		msg.AddU32(remove);
		msg.AddU32(creature->getID());
		msg.AddString(creature->getName());
	}

	if (creature->isHealthHidden()) {
		msg.AddByte(0x00);
	} else {
		msg.AddByte((int32_t)std::ceil(((float)creature->getHealth()) * 100 / std::max<int32_t>(creature->getMaxHealth(), 1)));
	}

	msg.AddByte((uint8_t)creature->getDirection());

	if (!creature->isInGhostMode() && !creature->isInvisible()) {
		AddCreatureOutfit(msg, creature, creature->getCurrentOutfit());
	} else {
		static Outfit_t outfit;
		AddCreatureOutfit(msg, creature, outfit);
	}

	LightInfo lightInfo;
	creature->getCreatureLight(lightInfo);
	msg.AddByte(player->isAccessPlayer() ? 0xFF : lightInfo.level);
	msg.AddByte(lightInfo.color);

	msg.AddU16(creature->getStepSpeed());

	msg.AddByte(player->getSkullClient(otherPlayer));
	msg.AddByte(player->getPartyShield(otherPlayer));
}

void ProtocolGame::AddPlayerStats(NetworkMessage& msg)
{
	msg.AddByte(0xA0);

	msg.AddU16(player->getHealth());
	msg.AddU16(player->getPlayerInfo(PLAYERINFO_MAXHEALTH));
	msg.AddU16(uint32_t(player->getFreeCapacity()));

	msg.AddU32(player->getExperience());

	#ifdef __PROTOCOL_76__
	msg.AddU16(player->getPlayerInfo(PLAYERINFO_LEVEL));
	#else
	msg.AddByte(player->getPlayerInfo(PLAYERINFO_LEVEL));
	#endif
	msg.AddByte(player->getPlayerInfo(PLAYERINFO_LEVELPERCENT));

	msg.AddU16(player->getMana());
	msg.AddU16(player->getPlayerInfo(PLAYERINFO_MAXMANA));

	msg.AddByte(player->getMagicLevel());
	msg.AddByte(player->getPlayerInfo(PLAYERINFO_MAGICLEVELPERCENT));

	#ifdef __PROTOCOL_76__
	msg.AddByte(player->getPlayerInfo(PLAYERINFO_SOUL));
	#endif
}

void ProtocolGame::AddPlayerSkills(NetworkMessage& msg)
{
	msg.AddByte(0xA1);

	msg.AddByte(std::min<int32_t>(0xFF, player->getSkill(SKILL_FIST, SKILL_LEVEL)));
	msg.AddByte(player->getSkill(SKILL_FIST, SKILL_PERCENT));

	msg.AddByte(std::min<int32_t>(0xFF, player->getSkill(SKILL_CLUB, SKILL_LEVEL)));
	msg.AddByte(player->getSkill(SKILL_CLUB, SKILL_PERCENT));

	msg.AddByte(std::min<int32_t>(0xFF, player->getSkill(SKILL_SWORD, SKILL_LEVEL)));
	msg.AddByte(player->getSkill(SKILL_SWORD, SKILL_PERCENT));

	msg.AddByte(std::min<int32_t>(0xFF, player->getSkill(SKILL_AXE, SKILL_LEVEL)));
	msg.AddByte(player->getSkill(SKILL_AXE, SKILL_PERCENT));

	msg.AddByte(std::min<int32_t>(0xFF, player->getSkill(SKILL_DIST, SKILL_LEVEL)));
	msg.AddByte(player->getSkill(SKILL_DIST, SKILL_PERCENT));

	msg.AddByte(std::min<int32_t>(0xFF, player->getSkill(SKILL_SHIELD, SKILL_LEVEL)));
	msg.AddByte(player->getSkill(SKILL_SHIELD, SKILL_PERCENT));

	msg.AddByte(std::min<int32_t>(0xFF, player->getSkill(SKILL_FISH, SKILL_LEVEL)));
	msg.AddByte(player->getSkill(SKILL_FISH, SKILL_PERCENT));
}

void ProtocolGame::AddCreatureSpeak(NetworkMessage& msg, const Creature* creature, SpeakClasses type, const std::string& text, uint16_t channelId, Position* pos/* = nullptr*/)
{
	if (!creature) {
		return;
	}

	msg.AddByte(0xAA);

	#ifdef __PROTOCOL_77__
	static uint32_t statementId = 0;
	msg.AddU32(++statementId); // statement id
	#endif

	if (type == SPEAK_CHANNEL_R2) {
		msg.AddU16(0x00);
		type = SPEAK_CHANNEL_R1;
	} else {
		msg.AddString(creature->getName());
	}

	msg.AddByte(type);

	switch (type) {
		case SPEAK_SAY:
		case SPEAK_WHISPER:
		case SPEAK_YELL:
		case SPEAK_MONSTER_SAY:
		case SPEAK_MONSTER_YELL: {
			if (pos) {
				msg.AddPosition(*pos);
			} else {
				msg.AddPosition(creature->getPosition());
			}

			break;
		}

		case SPEAK_CHANNEL_Y:
		case SPEAK_CHANNEL_R1:
		case SPEAK_CHANNEL_O:
			msg.AddU16(channelId);
			break;

		default:
			break;
	}

	msg.AddString(text);
}

void ProtocolGame::AddCreatureHealth(NetworkMessage& msg, const Creature* creature)
{
	msg.AddByte(0x8C);
	msg.AddU32(creature->getID());

	if (creature->isHealthHidden()) {
		msg.AddByte(0x00);
	} else {
		msg.AddByte((int32_t)std::ceil(((float)creature->getHealth()) * 100 / std::max<int32_t>(creature->getMaxHealth(), 1)));
	}
}

void ProtocolGame::AddCreatureOutfit(NetworkMessage& msg, const Creature* creature, const Outfit_t& outfit)
{
	#ifdef __PROTOCOL_77__
	msg.AddU16(outfit.lookType);
	#else
	msg.AddByte(outfit.lookType);
	#endif

	if (outfit.lookType != 0) {
		msg.AddByte(outfit.lookHead);
		msg.AddByte(outfit.lookBody);
		msg.AddByte(outfit.lookLegs);
		msg.AddByte(outfit.lookFeet);
	} else {
		msg.AddItemId(outfit.lookTypeEx);
	}
}

void ProtocolGame::AddWorldLight(NetworkMessage& msg, const LightInfo& lightInfo)
{
	msg.AddByte(0x82);
	msg.AddByte((player->isAccessPlayer() ? 0xFF : lightInfo.level));
	msg.AddByte(lightInfo.color);
}

void ProtocolGame::AddCreatureLight(NetworkMessage& msg, const Creature* creature)
{
	LightInfo lightInfo;
	creature->getCreatureLight(lightInfo);

	msg.AddByte(0x8D);
	msg.AddU32(creature->getID());
	msg.AddByte((player->isAccessPlayer() ? 0xFF : lightInfo.level));
	msg.AddByte(lightInfo.color);
}

//tile
void ProtocolGame::AddTileItem(NetworkMessage& msg, const Position& pos, uint32_t stackpos, const Item* item)
{
	if (stackpos >= 10) {
		return;
	}

	msg.AddByte(0x6A);
	msg.AddPosition(pos);
	msg.AddItem(item);
}

void ProtocolGame::AddTileCreature(NetworkMessage& msg, const Position& pos, uint32_t stackpos, const Creature* creature)
{
	if (stackpos >= 10) {
		return;
	}

	msg.AddByte(0x6A);
	msg.AddPosition(pos);

	bool known;
	uint32_t removedKnown;
	checkCreatureAsKnown(creature->getID(), known, removedKnown);
	AddCreature(msg, creature, known, removedKnown);
}

void ProtocolGame::UpdateTileItem(NetworkMessage& msg, const Position& pos, uint32_t stackpos, const Item* item)
{
	if (stackpos >= 10) {
		return;
	}

	msg.AddByte(0x6B);
	msg.AddPosition(pos);
	msg.AddByte(stackpos);
	msg.AddItem(item);
}

void ProtocolGame::RemoveTileItem(NetworkMessage& msg, const Position& pos, uint32_t stackpos)
{
	if (stackpos >= 10) {
		return;
	}

	msg.AddByte(0x6C);
	msg.AddPosition(pos);
	msg.AddByte(stackpos);
}

void ProtocolGame::MoveUpCreature(NetworkMessage& msg, const Creature* creature, const Position& newPos, const Position& oldPos, uint32_t oldStackPos)
{
	if (creature != player) {
		return;
	}

	//floor change up
	msg.AddByte(0xBE);

	//going to surface
	if (newPos.z == 7) {
		int32_t skip = -1;
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 5, 18, 14, 3, skip); //(floor 7 and 6 already set)
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 4, 18, 14, 4, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 3, 18, 14, 5, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 2, 18, 14, 6, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 1, 18, 14, 7, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 0, 18, 14, 8, skip);

		if (skip >= 0) {
			msg.AddByte(skip);
			msg.AddByte(0xFF);
		}
	}
	//underground, going one floor up (still underground)
	else if (newPos.z > 7) {
		int32_t skip = -1;
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, oldPos.getZ() - 3, 18, 14, 3, skip);

		if (skip >= 0) {
			msg.AddByte(skip);
			msg.AddByte(0xFF);
		}
	}

	//moving up a floor up makes us out of sync
	//west
	msg.AddByte(0x68);
	GetMapDescription(oldPos.x - 8, oldPos.y - 5, newPos.z, 1, 14, msg);

	//north
	msg.AddByte(0x65);
	GetMapDescription(oldPos.x - 8, oldPos.y - 6, newPos.z, 18, 1, msg);
}

void ProtocolGame::MoveDownCreature(NetworkMessage& msg, const Creature* creature, const Position& newPos, const Position& oldPos, uint32_t oldStackPos)
{
	if (creature != player) {
		return;
	}

	//floor change down
	msg.AddByte(0xBF);

	//going from surface to underground
	if (newPos.z == 8) {
		int32_t skip = -1;

		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, newPos.z, 18, 14, -1, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, newPos.z + 1, 18, 14, -2, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, newPos.z + 2, 18, 14, -3, skip);

		if (skip >= 0) {
			msg.AddByte(skip);
			msg.AddByte(0xFF);
		}
	}
	//going further down
	else if (newPos.z > oldPos.z && newPos.z > 8 && newPos.z < 14) {
		int32_t skip = -1;
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, newPos.z + 2, 18, 14, -3, skip);

		if (skip >= 0) {
			msg.AddByte(skip);
			msg.AddByte(0xFF);
		}
	}

	//moving down a floor makes us out of sync
	//east
	msg.AddByte(0x66);
	GetMapDescription(oldPos.x + 9, oldPos.y - 7, newPos.z, 1, 14, msg);

	//south
	msg.AddByte(0x67);
	GetMapDescription(oldPos.x - 8, oldPos.y + 7, newPos.z, 18, 1, msg);
}
