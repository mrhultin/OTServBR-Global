/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019  Mark Samman <mark.samman@gmail.com>
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

#include "../../otpch.h"

#include "npcs.h"
#include "npc.h"
#include "../combat/spells.h"
#include "../combat/combat.h"
#include "../../items/weapons/weapons.h"
#include "../../config/configmanager.h"
#include "../../game/game.h"
#include "../creature.h"
#include "../../utils/enums.h"

#include "../../utils/pugicast.h"

extern Game g_game;
extern Spells* g_spells;
extern Npcs g_npcs;
extern ConfigManager g_config;

spellBlockNpc_t::~spellBlockNpc_t()
{
	if (combatSpell) {
		delete spell;
	}
}

bool Npcs::loadFromXml(bool reloading /*= false*/)
{
	unloadedNpcs = {};
	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file("data/npc/npcs.xml");
	if (!result) {
		printXMLError("Error - Npcs::loadFromXml", "data/npc/npcs.xml", result);
		return false;
	}

	loaded = true;
	

	for (auto npcNode : doc.child("npcs").children()) {
		std::string name = asLowerCaseString(npcNode.attribute("name").as_string());
		std::string file = "data/npc/" + std::string(npcNode.attribute("file").as_string());
		loadNpc(file, name, reloading);
	}
	return true;
}

bool NpcType::canSpawn(const Position& pos)
{
	bool canSpawn = true;
	bool isDay = g_game.gameIsDay();

	if ((isDay && info.respawnType.period == RESPAWNPERIOD_NIGHT) ||
		(!isDay && info.respawnType.period == RESPAWNPERIOD_DAY)) {
		// It will ignore day and night if underground
		canSpawn = (pos.z > 7 && info.respawnType.underground);
	}

	return canSpawn;
}

bool Npcs::reload()
{
	loaded = false;

	scriptInterface.reset();

	return loadFromXml(true);
}

ConditionDamage* Npcs::getDamageCondition(ConditionType_t conditionType,
		int32_t maxDamage, int32_t minDamage, int32_t startDamage, uint32_t tickInterval)
{
	ConditionDamage* condition = static_cast<ConditionDamage*>(Condition::createCondition(CONDITIONID_COMBAT, conditionType, 0, 0));
	condition->setParam(CONDITION_PARAM_TICKINTERVAL, tickInterval);
	condition->setParam(CONDITION_PARAM_MINVALUE, minDamage);
	condition->setParam(CONDITION_PARAM_MAXVALUE, maxDamage);
	condition->setParam(CONDITION_PARAM_STARTVALUE, startDamage);
	condition->setParam(CONDITION_PARAM_DELAYED, 1);
	return condition;
}

bool Npcs::deserializeSpell(const pugi::xml_node& node, spellBlockNpc_t& sb, const std::string& description)
{
	std::string name;
	std::string scriptName;
	bool isScripted;

	pugi::xml_attribute attr;
	if ((attr = node.attribute("script"))) {
		scriptName = attr.as_string();
		isScripted = true;
	} else if ((attr = node.attribute("name"))) {
		name = attr.as_string();
		isScripted = false;
	} else {
		return false;
	}

	if ((attr = node.attribute("speed")) || (attr = node.attribute("interval"))) {
		sb.speed = std::max<int32_t>(1, pugi::cast<int32_t>(attr.value()));
	}

	if ((attr = node.attribute("chance"))) {
		uint32_t chance = pugi::cast<uint32_t>(attr.value());
		if (chance > 100) {
			chance = 100;
		}
		sb.chance = chance;
	}

	if ((attr = node.attribute("range"))) {
		uint32_t range = pugi::cast<uint32_t>(attr.value());
		if (range > (Map::maxViewportX * 2)) {
			range = Map::maxViewportX * 2;
		}
		sb.range = range;
	}

	if ((attr = node.attribute("min"))) {
		sb.minCombatValue = pugi::cast<int32_t>(attr.value());
	}

	if ((attr = node.attribute("max"))) {
		sb.maxCombatValue = pugi::cast<int32_t>(attr.value());

		//normalize values
		if (std::abs(sb.minCombatValue) > std::abs(sb.maxCombatValue)) {
			int32_t value = sb.maxCombatValue;
			sb.maxCombatValue = sb.minCombatValue;
			sb.minCombatValue = value;
		}
	}

	if (auto spell = g_spells->getSpellByName(name)) {
		sb.spell = spell;
		return true;
	}

	CombatSpell* combatSpell = nullptr;
	bool needTarget = false;
	bool needDirection = false;

	if (isScripted) {
		if ((attr = node.attribute("direction"))) {
			needDirection = attr.as_bool();
		}

		if ((attr = node.attribute("target"))) {
			needTarget = attr.as_bool();
		}

		std::unique_ptr<CombatSpell> combatSpellPtr(new CombatSpell(nullptr, needTarget, needDirection));
		if (!combatSpellPtr->loadScript("data/" + g_spells->getScriptBaseName() + "/scripts/" + scriptName)) {
			return false;
		}

		if (!combatSpellPtr->loadScriptCombat()) {
			return false;
		}

		combatSpell = combatSpellPtr.release();
		combatSpell->getCombat()->setPlayerCombatValues(COMBAT_FORMULA_DAMAGE, sb.minCombatValue, 0, sb.maxCombatValue, 0);
	} else {
		Combat* combat = new Combat;
		if ((attr = node.attribute("length"))) {
			int32_t length = pugi::cast<int32_t>(attr.value());
			if (length > 0) {
				int32_t spread = 3;

				//need direction spell
				if ((attr = node.attribute("spread"))) {
					spread = std::max<int32_t>(0, pugi::cast<int32_t>(attr.value()));
				}

				AreaCombat* area = new AreaCombat();
				area->setupArea(length, spread);
				combat->setArea(area);

				needDirection = true;
			}
		}

		if ((attr = node.attribute("radius"))) {
			int32_t radius = pugi::cast<int32_t>(attr.value());

			//target spell
			if ((attr = node.attribute("target"))) {
				needTarget = attr.as_bool();
			}

			AreaCombat* area = new AreaCombat();
			area->setupArea(radius);
			combat->setArea(area);
		}

		std::string tmpName = asLowerCaseString(name);

		if (tmpName == "melee") {
			sb.isMelee = true;

			pugi::xml_attribute attackAttribute, skillAttribute;
			if ((attackAttribute = node.attribute("attack")) && (skillAttribute = node.attribute("skill"))) {
				sb.minCombatValue = 0;
				sb.maxCombatValue = -Weapons::getMaxMeleeDamage(pugi::cast<int32_t>(skillAttribute.value()), pugi::cast<int32_t>(attackAttribute.value()));
			}

			ConditionType_t conditionType = CONDITION_NONE;
			int32_t minDamage = 0;
			int32_t maxDamage = 0;
			uint32_t tickInterval = 2000;

			if ((attr = node.attribute("fire"))) {
				conditionType = CONDITION_FIRE;

				minDamage = pugi::cast<int32_t>(attr.value());
				maxDamage = minDamage;
				tickInterval = 9000;
			} else if ((attr = node.attribute("poison"))) {
				conditionType = CONDITION_POISON;

				minDamage = pugi::cast<int32_t>(attr.value());
				maxDamage = minDamage;
				tickInterval = 5000;
			} else if ((attr = node.attribute("energy"))) {
				conditionType = CONDITION_ENERGY;

				minDamage = pugi::cast<int32_t>(attr.value());
				maxDamage = minDamage;
				tickInterval = 10000;
			} else if ((attr = node.attribute("drown"))) {
				conditionType = CONDITION_DROWN;

				minDamage = pugi::cast<int32_t>(attr.value());
				maxDamage = minDamage;
				tickInterval = 5000;
			} else if ((attr = node.attribute("freeze"))) {
				conditionType = CONDITION_FREEZING;

				minDamage = pugi::cast<int32_t>(attr.value());
				maxDamage = minDamage;
				tickInterval = 8000;
			} else if ((attr = node.attribute("dazzle"))) {
				conditionType = CONDITION_DAZZLED;

				minDamage = pugi::cast<int32_t>(attr.value());
				maxDamage = minDamage;
				tickInterval = 10000;
			} else if ((attr = node.attribute("curse"))) {
				conditionType = CONDITION_CURSED;

				minDamage = pugi::cast<int32_t>(attr.value());
				maxDamage = minDamage;
				tickInterval = 4000;
			} else if ((attr = node.attribute("bleed")) || (attr = node.attribute("physical"))) {
				conditionType = CONDITION_BLEEDING;
				tickInterval = 5000;
			}

			if ((attr = node.attribute("tick"))) {
				int32_t value = pugi::cast<int32_t>(attr.value());
				if (value > 0) {
					tickInterval = value;
				}
			}

			if (conditionType != CONDITION_NONE) {
				Condition* condition = getDamageCondition(conditionType, maxDamage, minDamage, 0, tickInterval);
				combat->addCondition(condition);
			}

			sb.range = 1;
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_PHYSICALDAMAGE);
			combat->setParam(COMBAT_PARAM_BLOCKARMOR, 1);
			combat->setParam(COMBAT_PARAM_BLOCKSHIELD, 1);
			combat->setOrigin(ORIGIN_MELEE);
		} else if (tmpName == "physical") {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_PHYSICALDAMAGE);
			combat->setParam(COMBAT_PARAM_BLOCKARMOR, 1);
			combat->setOrigin(ORIGIN_RANGED);
		} else if (tmpName == "bleed") {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_PHYSICALDAMAGE);
		} else if (tmpName == "poison" || tmpName == "earth") {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_EARTHDAMAGE);
		} else if (tmpName == "fire") {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_FIREDAMAGE);
		} else if (tmpName == "energy") {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_ENERGYDAMAGE);
		} else if (tmpName == "drown") {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_DROWNDAMAGE);
		} else if (tmpName == "ice") {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_ICEDAMAGE);
		} else if (tmpName == "holy") {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_HOLYDAMAGE);
		} else if (tmpName == "death") {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_DEATHDAMAGE);
		} else if (tmpName == "lifedrain") {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_LIFEDRAIN);
		} else if (tmpName == "manadrain") {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_MANADRAIN);
		} else if (tmpName == "healing") {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_HEALING);
			combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
		} else if (tmpName == "speed") {
			int32_t speedChange = 0;
			int32_t duration = 10000;

			if ((attr = node.attribute("duration"))) {
				duration = pugi::cast<int32_t>(attr.value());
			}

			if ((attr = node.attribute("speedchange"))) {
				speedChange = pugi::cast<int32_t>(attr.value());
				if (speedChange < -1000) {
					//cant be slower than 100%
					speedChange = -1000;
				}
			}

			ConditionType_t conditionType;
			if (speedChange > 0) {
				conditionType = CONDITION_HASTE;
				combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
			} else {
				conditionType = CONDITION_PARALYZE;
			}

			ConditionSpeed* condition = static_cast<ConditionSpeed*>(Condition::createCondition(CONDITIONID_COMBAT, conditionType, duration, 0));
			condition->setFormulaVars(speedChange / 1000.0, 0, speedChange / 1000.0, 0);
			combat->addCondition(condition);
		} else if (tmpName == "outfit") {
			int32_t duration = 10000;

			if ((attr = node.attribute("duration"))) {
				duration = pugi::cast<int32_t>(attr.value());
			}

			if ((attr = node.attribute("npc"))) {
				NpcType* npcType = g_npcs.getNpcType(attr.as_string());
				if (npcType) {
					ConditionOutfit* condition = static_cast<ConditionOutfit*>(Condition::createCondition(CONDITIONID_COMBAT, CONDITION_OUTFIT, duration, 0));
					condition->setOutfit(npcType->info.outfit);
					combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
					combat->addCondition(condition);
				}
			} else if ((attr = node.attribute("item"))) {
				Outfit_t outfit;
				outfit.lookTypeEx = pugi::cast<uint16_t>(attr.value());

				ConditionOutfit* condition = static_cast<ConditionOutfit*>(Condition::createCondition(CONDITIONID_COMBAT, CONDITION_OUTFIT, duration, 0));
				condition->setOutfit(outfit);
				combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
				combat->addCondition(condition);
			}
		} else if (tmpName == "invisible") {
			int32_t duration = 10000;

			if ((attr = node.attribute("duration"))) {
				duration = pugi::cast<int32_t>(attr.value());
			}

			Condition* condition = Condition::createCondition(CONDITIONID_COMBAT, CONDITION_INVISIBLE, duration, 0);
			combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
			combat->addCondition(condition);
		} else if (tmpName == "drunk") {
			int32_t duration = 10000;

			if ((attr = node.attribute("duration"))) {
				duration = pugi::cast<int32_t>(attr.value());
			}

			Condition* condition = Condition::createCondition(CONDITIONID_COMBAT, CONDITION_DRUNK, duration, 0);
			combat->addCondition(condition);
		} else if (tmpName == "firefield") {
			combat->setParam(COMBAT_PARAM_CREATEITEM, ITEM_FIREFIELD_PVP_FULL);
		} else if (tmpName == "poisonfield") {
			combat->setParam(COMBAT_PARAM_CREATEITEM, ITEM_POISONFIELD_PVP);
		} else if (tmpName == "energyfield") {
			combat->setParam(COMBAT_PARAM_CREATEITEM, ITEM_ENERGYFIELD_PVP);
		} else if (tmpName == "firecondition" || tmpName == "energycondition" ||
				   tmpName == "earthcondition" || tmpName == "poisoncondition" ||
				   tmpName == "icecondition" || tmpName == "freezecondition" ||
				   tmpName == "deathcondition" || tmpName == "cursecondition" ||
				   tmpName == "holycondition" || tmpName == "dazzlecondition" ||
				   tmpName == "drowncondition" || tmpName == "bleedcondition" ||
				   tmpName == "physicalcondition") {
			ConditionType_t conditionType = CONDITION_NONE;
			uint32_t tickInterval = 2000;

			if (tmpName == "firecondition") {
				conditionType = CONDITION_FIRE;
				tickInterval = 10000;
			} else if (tmpName == "poisoncondition" || tmpName == "earthcondition") {
				conditionType = CONDITION_POISON;
				tickInterval = 5000;
			} else if (tmpName == "energycondition") {
				conditionType = CONDITION_ENERGY;
				tickInterval = 10000;
			} else if (tmpName == "drowncondition") {
				conditionType = CONDITION_DROWN;
				tickInterval = 5000;
			} else if (tmpName == "freezecondition" || tmpName == "icecondition") {
				conditionType = CONDITION_FREEZING;
				tickInterval = 10000;
			} else if (tmpName == "cursecondition" || tmpName == "deathcondition") {
				conditionType = CONDITION_CURSED;
				tickInterval = 4000;
			} else if (tmpName == "dazzlecondition" || tmpName == "holycondition") {
				conditionType = CONDITION_DAZZLED;
				tickInterval = 10000;
			} else if (tmpName == "physicalcondition" || tmpName == "bleedcondition") {
				conditionType = CONDITION_BLEEDING;
				tickInterval = 5000;
			}

			if ((attr = node.attribute("tick"))) {
				int32_t value = pugi::cast<int32_t>(attr.value());
				if (value > 0) {
					tickInterval = value;
				}
			}

			int32_t minDamage = std::abs(sb.minCombatValue);
			int32_t maxDamage = std::abs(sb.maxCombatValue);
			int32_t startDamage = 0;

			if ((attr = node.attribute("start"))) {
				int32_t value = std::abs(pugi::cast<int32_t>(attr.value()));
				if (value <= minDamage) {
					startDamage = value;
				}
			}

			Condition* condition = getDamageCondition(conditionType, maxDamage, minDamage, startDamage, tickInterval);
			combat->addCondition(condition);
		} else if (tmpName == "strength") {
			//
		} else if (tmpName == "effect") {
			//
		} else {
			SPDLOG_ERROR("[Npcs::deserializeSpell] - {} unknown spell name: {}",
                         description, name);
			delete combat;
			return false;
		}

		combat->setPlayerCombatValues(COMBAT_FORMULA_DAMAGE, sb.minCombatValue, 0, sb.maxCombatValue, 0);
		combatSpell = new CombatSpell(combat, needTarget, needDirection);

		for (auto attributeNode : node.children()) {
			if ((attr = attributeNode.attribute("key"))) {
				const char* value = attr.value();
				if (strcasecmp(value, "shooteffect") == 0) {
					if ((attr = attributeNode.attribute("value"))) {
						ShootType_t shoot = getShootType(asLowerCaseString(attr.as_string()));
						if (shoot != CONST_ANI_NONE) {
							combat->setParam(COMBAT_PARAM_DISTANCEEFFECT, shoot);
						} else {
							SPDLOG_WARN("[Npcs::deserializeSpell] - "
                                        "{} unknown shootEffect: {}",
                                        description, attr.as_string());
						}
					}
				} else if (strcasecmp(value, "areaeffect") == 0) {
					if ((attr = attributeNode.attribute("value"))) {
						MagicEffectClasses effect = getMagicEffect(asLowerCaseString(attr.as_string()));
						if (effect != CONST_ME_NONE) {
							combat->setParam(COMBAT_PARAM_EFFECT, effect);
						} else {
							SPDLOG_WARN("[Npcs::deserializeSpell] - "
                                        "{} unknown areaEffect: {}",
                                        description, attr.as_string());
						}
					}
				} else {
					SPDLOG_WARN("[Npcs::deserializeSpells] - "
                                "Effect type {} does not exist",
                                attr.as_string());
				}
			}
		}
	}

	sb.spell = combatSpell;
	if (combatSpell) {
		sb.combatSpell = true;
	}
	return true;
}

bool Npcs::deserializeSpell(NpcSpell* spell, spellBlockNpc_t& sb, const std::string& description)
{
	if (!spell->scriptName.empty()) {
		spell->isScripted = true;
	} else if (!spell->name.empty()) {
		spell->isScripted = false;
	} else {
		return false;
	}

	sb.speed = spell->interval;
	sb.chance = std::min((int) spell->chance, 100);
	sb.range = std::min((int) spell->range, Map::maxViewportX * 2);
	sb.minCombatValue = std::min(spell->minCombatValue, spell->maxCombatValue);
	sb.maxCombatValue = std::max(spell->minCombatValue, spell->maxCombatValue);
	sb.spell = g_spells->getSpellByName(spell->name);

	if (sb.spell) {
		return true;
	}

	CombatSpell* combatSpell = nullptr;

	if (spell->isScripted) {
		std::unique_ptr<CombatSpell> combatSpellPtr(new CombatSpell(nullptr, spell->needTarget, spell->needDirection));
		if (!combatSpellPtr->loadScript("data/" + g_spells->getScriptBaseName() + "/scripts/" + spell->scriptName)) {
			SPDLOG_ERROR("[Npcs::deserializeSpell] - Cannot find file: {}",
                         spell->scriptName);
			return false;
		}

		if (!combatSpellPtr->loadScriptCombat()) {
			return false;
		}

		combatSpell = combatSpellPtr.release();
		combatSpell->getCombat()->setPlayerCombatValues(COMBAT_FORMULA_DAMAGE, sb.minCombatValue, 0, sb.maxCombatValue, 0);
	} else {
		std::unique_ptr<Combat> combat{ new Combat };
		sb.combatSpell = true;

		if (spell->length > 0) {
			spell->spread = std::max<int32_t>(0, spell->spread);

			AreaCombat* area = new AreaCombat();
			area->setupArea(spell->length, spell->spread);
			combat->setArea(area);

			spell->needDirection = true;
		}

		if (spell->radius > 0) {
			AreaCombat* area = new AreaCombat();
			area->setupArea(spell->radius);
			combat->setArea(area);
		}

		std::string tmpName = asLowerCaseString(spell->name);

		if (tmpName == "melee") {
			sb.isMelee = true;

			if (spell->attack > 0 && spell->skill > 0) {
				sb.minCombatValue = 0;
				sb.maxCombatValue = -Weapons::getMaxMeleeDamage(spell->skill, spell->attack);
			}

			sb.range = 1;
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_PHYSICALDAMAGE);
			combat->setParam(COMBAT_PARAM_BLOCKARMOR, 1);
			combat->setParam(COMBAT_PARAM_BLOCKSHIELD, 1);
			combat->setOrigin(ORIGIN_MELEE);
		} else if (tmpName == "combat") {
			if (spell->combatType == COMBAT_PHYSICALDAMAGE) {
				combat->setParam(COMBAT_PARAM_BLOCKARMOR, 1);
				combat->setOrigin(ORIGIN_RANGED);
			} else if (spell->combatType == COMBAT_HEALING) {
				combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
			}
			combat->setParam(COMBAT_PARAM_TYPE, spell->combatType);
		} else if (tmpName == "speed") {
			int32_t speedChange = 0;
			int32_t duration = 10000;

			if (spell->duration != 0) {
				duration = spell->duration;
			}

			if (spell->speedChange != 0) {
				speedChange = spell->speedChange;
				if (speedChange < -1000) {
					//cant be slower than 100%
					speedChange = -1000;
				}
			}

			ConditionType_t conditionType;
			if (speedChange > 0) {
				conditionType = CONDITION_HASTE;
				combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
			} else {
				conditionType = CONDITION_PARALYZE;
			}

			ConditionSpeed* condition = static_cast<ConditionSpeed*>(Condition::createCondition(CONDITIONID_COMBAT, conditionType, duration, 0));
			condition->setFormulaVars(speedChange / 1000.0, 0, speedChange / 1000.0, 0);
			combat->addCondition(condition);
		} else if (tmpName == "outfit") {
			int32_t duration = 10000;

			if (spell->duration != 0) {
				duration = spell->duration;
			}

			ConditionOutfit* condition = static_cast<ConditionOutfit*>(Condition::createCondition(CONDITIONID_COMBAT, CONDITION_OUTFIT, duration, 0));
			
			if (spell->outfitNpc != "") {
//monster				condition->setLazyNpcOutfit(spell->outfitNpc);
			} else if (spell->outfitItem > 0) {
				Outfit_t outfit;
				outfit.lookTypeEx = spell->outfitItem;
				condition->setOutfit(outfit);
			} else {
				SPDLOG_ERROR("[Npcs::deserializeSpell] - "
                             "Missing outfit npc or item in outfit spell for: {}",
                            description);
				return false;
			}

			combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
			combat->addCondition(condition);
		} else if (tmpName == "invisible") {
			int32_t duration = 10000;

			if (spell->duration != 0) {
				duration = spell->duration;
			}

			Condition* condition = Condition::createCondition(CONDITIONID_COMBAT, CONDITION_INVISIBLE, duration, 0);
			combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
			combat->addCondition(condition);
		} else if (tmpName == "drunk") {
			int32_t duration = 10000;

			if (spell->duration != 0) {
				duration = spell->duration;
			}

			Condition* condition = Condition::createCondition(CONDITIONID_COMBAT, CONDITION_DRUNK, duration, 0);
			combat->addCondition(condition);
		} else if (tmpName == "firefield") {
			combat->setParam(COMBAT_PARAM_CREATEITEM, ITEM_FIREFIELD_PVP_FULL);
		} else if (tmpName == "poisonfield") {
			combat->setParam(COMBAT_PARAM_CREATEITEM, ITEM_POISONFIELD_PVP);
		} else if (tmpName == "energyfield") {
			combat->setParam(COMBAT_PARAM_CREATEITEM, ITEM_ENERGYFIELD_PVP);
		} else if (tmpName == "condition") {
			if (spell->conditionType == CONDITION_NONE) {
				SPDLOG_ERROR("[Npcs::deserializeSpell] - "
                             "{} condition is not set for: {}",
                             description, spell->name);
			}
		} else if (tmpName == "strength") {
			//
		} else if (tmpName == "effect") {
			//
		} else {
			SPDLOG_ERROR("[Npcs::deserializeSpell] - "
                         "{} unknown spell name: {}",
                         description, spell->name);
		}

		if (spell->shoot != CONST_ANI_NONE) {
			combat->setParam(COMBAT_PARAM_DISTANCEEFFECT, spell->shoot);
		}

		if (spell->effect != CONST_ME_NONE) {
			combat->setParam(COMBAT_PARAM_EFFECT, spell->effect);
		}

		// If a spell has a condition, it always applies, no matter what kind of spell it is
		if (spell->conditionType != CONDITION_NONE) {
			int32_t minDamage = std::abs(spell->conditionMinDamage);
			int32_t maxDamage = std::abs(spell->conditionMaxDamage);
			int32_t startDamage = std::abs(spell->conditionStartDamage);
			uint32_t tickInterval = 2000;

			if (spell->tickInterval > 0) {
				tickInterval = spell->tickInterval;
			}

			if (startDamage > minDamage) {
				startDamage = 0;
			}

			if (maxDamage == 0) {
				maxDamage = minDamage;
			}

			Condition* condition = getDamageCondition(spell->conditionType, maxDamage, minDamage, startDamage, tickInterval);
			combat->addCondition(condition);
		}

		combat->setPlayerCombatValues(COMBAT_FORMULA_DAMAGE, sb.minCombatValue, 0, sb.maxCombatValue, 0);
		combatSpell = new CombatSpell(combat.release(), spell->needTarget, spell->needDirection);
	}

	sb.spell = combatSpell;
	if (combatSpell) {
		sb.combatSpell = true;
	}
	return true;
}

NpcType* Npcs::loadNpc(const std::string& file, const std::string& npcName, bool reloading /*= false*/)
{
	NpcType* npcType = nullptr;

	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file(file.c_str());
	if (!result) {
		printXMLError("Error - Npcs::loadNpc", file, result);
		return nullptr;
	}

	pugi::xml_node npcNode = doc.child("npc");
	if (!npcNode) {
		SPDLOG_ERROR("[Npcs::loadNpc] - Missing npc node in: {}",
                     file);
		return nullptr;
	}

	pugi::xml_attribute attr;
	if (!(attr = npcNode.attribute("name"))) {
		SPDLOG_ERROR("[Npcs::loadNpc] - Missing name in: {}", file);
		return nullptr;
	}

	if (reloading) {
		auto it = npcs.find(npcName);
		if (it != npcs.end()) {
			npcType = &it->second;
			npcType->info = {};
		}
	}
	if (!npcType) {
		npcType = &npcs[npcName];
	}

	npcType->name = attr.as_string();
	npcType->nameDescription = npcType->name;

	pugi::xml_attribute coin;
	if ((coin = npcNode.attribute("currency"))) {
		const ItemType& it = Item::items[pugi::cast<uint16_t>(coin.value())];
		npcType->info.currencyServerId = it.id;
		npcType->info.currencyClientId = it.clientId;
	} else {
		const ItemType& it = Item::items[ITEM_GOLD_COIN];
		npcType->info.currencyServerId = it.id;
		npcType->info.currencyClientId = it.clientId;
	}

	if ((attr = npcNode.attribute("speechbubble"))) {
		npcType->info.speechBubble = pugi::cast<uint32_t>(attr.value());
	}

	if ((attr = npcNode.attribute("experience"))) {
		npcType->info.experience = pugi::cast<uint64_t>(attr.value());
	}

	if ((attr = npcNode.attribute("speed"))) {
		npcType->info.baseSpeed = pugi::cast<int32_t>(attr.value());
	}

	if ((attr = npcNode.attribute("walkinterval"))) {
		npcType->info.walkInterval = pugi::cast<uint32_t>(attr.value());
	}

	if ((attr = npcNode.attribute("walkradius"))) {
		npcType->info.walkRadius = pugi::cast<int32_t>(attr.value());
	}

	if ((attr = npcNode.attribute("skull"))) {
		npcType->info.skull = getSkullType(asLowerCaseString(attr.as_string()));
	}

	if ((attr = npcNode.attribute("script"))) {
		if (!scriptInterface) {
			scriptInterface.reset(new LuaScriptInterface("Npc Interface"));
			scriptInterface->initState();
		}

		std::string script = attr.as_string();
		if (scriptInterface->loadFile("data/npc/scripts/" + script) == 0) {
			npcType->info.scriptInterface = scriptInterface.get();
			npcType->info.creatureAppearEvent = scriptInterface->getEvent("onCreatureAppear");
			npcType->info.creatureDisappearEvent = scriptInterface->getEvent("onCreatureDisappear");
			npcType->info.creatureMoveEvent = scriptInterface->getEvent("onCreatureMove");
			npcType->info.creatureSayEvent = scriptInterface->getEvent("onCreatureSay");
			npcType->info.thinkEvent = scriptInterface->getEvent("onThink");
		} else {
			SPDLOG_WARN("[Npcs::loadNpc] - Can not load script: {}", script);
			SPDLOG_WARN("{}", scriptInterface->getLastLuaError());
		}
	}

	pugi::xml_node node;
	if ((node = npcNode.child("health"))) {
		if ((attr = node.attribute("now"))) {
			npcType->info.health = pugi::cast<int32_t>(attr.value());
		} else {
			SPDLOG_ERROR("[Npcs::loadNpc] - Missing health now. {}", file);
		}

		if ((attr = node.attribute("max"))) {
			npcType->info.healthMax = pugi::cast<int32_t>(attr.value());
		} else {
			SPDLOG_ERROR("[Npcs::loadNpc] Missing health max. {}", file);
		}
	}

	if ((node = npcNode.child("flags"))) {
		for (auto flagNode : node.children()) {
			attr = flagNode.first_attribute();
			const char* attrName = attr.name();
			if (strcasecmp(attrName, "summonable") == 0) {
				npcType->info.isSummonable = attr.as_bool();
			} else if (strcasecmp(attrName, "floorchange") == 0) {
				npcType->info.floorChange = attr.as_bool();
			} else if (strcasecmp(attrName, "attackable") == 0) {
				npcType->info.isAttackable = attr.as_bool();
			} else if (strcasecmp(attrName, "hostile") == 0) {
				npcType->info.isHostile = attr.as_bool();
			} else if (strcasecmp(attrName, "illusionable") == 0) {
				npcType->info.isIllusionable = attr.as_bool();
			} else if (strcasecmp(attrName, "convinceable") == 0) {
				npcType->info.isConvinceable = attr.as_bool();
			} else if (strcasecmp(attrName, "pushable") == 0) {
				npcType->info.pushable = attr.as_bool();
			} else if (strcasecmp(attrName, "canpushitems") == 0) {
				npcType->info.canPushItems = attr.as_bool();
			} else if (strcasecmp(attrName, "canpushcreatures") == 0) {
				npcType->info.canPushCreatures = attr.as_bool();
			} else if (strcasecmp(attrName, "staticattack") == 0) {
				uint32_t staticAttack = pugi::cast<uint32_t>(attr.value());
				if (staticAttack > 100) {
					SPDLOG_WARN("[Npcs::loadNpc] - "
                                "Staticattack greater than 100. {}", file);
					staticAttack = 100;
				}

				npcType->info.staticAttackChance = staticAttack;
			} else if (strcasecmp(attrName, "lightlevel") == 0) {
				npcType->info.light.level = pugi::cast<uint16_t>(attr.value());
			} else if (strcasecmp(attrName, "lightcolor") == 0) {
				npcType->info.light.color = pugi::cast<uint16_t>(attr.value());
			} else if (strcasecmp(attrName, "targetdistance") == 0) {
				npcType->info.targetDistance = std::max<int32_t>(1, pugi::cast<int32_t>(attr.value()));
			} else if (strcasecmp(attrName, "runonhealth") == 0) {
				npcType->info.runAwayHealth = pugi::cast<int32_t>(attr.value());
			} else if (strcasecmp(attrName, "hidehealth") == 0) {
				npcType->info.hiddenHealth = attr.as_bool();
			} else if (strcasecmp(attrName, "canwalkonenergy") == 0) {
				npcType->info.canWalkOnEnergy = attr.as_bool();
			} else if (strcasecmp(attrName, "canwalkonfire") == 0) {
				npcType->info.canWalkOnFire = attr.as_bool();
			} else if (strcasecmp(attrName, "canwalkonpoison") == 0) {
				npcType->info.canWalkOnPoison = attr.as_bool();
			} else if (strcasecmp(attrName, "respawntype") == 0) {
				SpawnType_t spawnType = getSpawnType(asLowerCaseString(attr.as_string()));
				if (spawnType == RESPAWN_IN_ALL) {
					npcType->info.respawnType.period = RESPAWNPERIOD_ALL;
				} else if (spawnType == RESPAWN_IN_DAY) {
					npcType->info.respawnType.period = RESPAWNPERIOD_DAY;
				} else if (spawnType == RESPAWN_IN_NIGHT) {
					npcType->info.respawnType.period = RESPAWNPERIOD_NIGHT;
				} else if (spawnType == RESPAWN_IN_DAY_CAVE) {
					npcType->info.respawnType.period = RESPAWNPERIOD_DAY;
					npcType->info.respawnType.underground = true;
				} else if (spawnType == RESPAWN_IN_NIGHT_CAVE) {
					npcType->info.respawnType.period = RESPAWNPERIOD_NIGHT;
					npcType->info.respawnType.underground = true;
				}
			} else {
				SPDLOG_WARN("[Npcs::loadNpc] - "
                            "Unknown flag attribute: {}. {}", attrName, file);
			}
		}

		//if a npc can push creatures,
		// it should not be pushable
		if (npcType->info.canPushCreatures) {
			npcType->info.pushable = false;
		}
	}

	if ((node = npcNode.child("targetchange"))) {
		if ((attr = node.attribute("speed")) || (attr = node.attribute("interval"))) {
			npcType->info.changeTargetSpeed = pugi::cast<uint32_t>(attr.value());
		} else {
			SPDLOG_WARN("[Npcs::loadNpc] - "
                        "Missing targetchange speed. {}", file);
		}

		if ((attr = node.attribute("chance"))) {
			npcType->info.changeTargetChance = pugi::cast<int32_t>(attr.value());
		} else {
			SPDLOG_WARN("[Npcs::loadNpc] - "
                        "Missing targetchange chance. {}", file);
		}
	}

	if ((node = npcNode.child("look"))) {
		if ((attr = node.attribute("type"))) {
			npcType->info.outfit.lookType = pugi::cast<uint16_t>(attr.value());

			if ((attr = node.attribute("head"))) {
				npcType->info.outfit.lookHead = pugi::cast<uint16_t>(attr.value());
			}

			if ((attr = node.attribute("body"))) {
				npcType->info.outfit.lookBody = pugi::cast<uint16_t>(attr.value());
			}

			if ((attr = node.attribute("legs"))) {
				npcType->info.outfit.lookLegs = pugi::cast<uint16_t>(attr.value());
			}

			if ((attr = node.attribute("feet"))) {
				npcType->info.outfit.lookFeet = pugi::cast<uint16_t>(attr.value());
			}

			if ((attr = node.attribute("addons"))) {
				npcType->info.outfit.lookAddons = pugi::cast<uint16_t>(attr.value());
			}
		} else if ((attr = node.attribute("typeex"))) {
			npcType->info.outfit.lookTypeEx = pugi::cast<uint16_t>(attr.value());
		} else {
			SPDLOG_WARN("[Npcs::loadNpc] - "
                        "Missing look type/typeex. {}", file);
		}

		if ((attr = node.attribute("mount"))) {
			npcType->info.outfit.lookMount = pugi::cast<uint16_t>(attr.value());
		}

		if ((attr = node.attribute("corpse"))) {
			npcType->info.lookcorpse = pugi::cast<uint16_t>(attr.value());
		}
	}

	if ((node = npcNode.child("attacks"))) {
		for (auto attackNode : node.children()) {
			spellBlockNpc_t sb;
			if (deserializeSpell(attackNode, sb, npcName)) {
				npcType->info.attackSpells.emplace_back(std::move(sb));
			} else {
				SPDLOG_WARN("[Npcs::loadNpc] - "
                            "Cant load spell. {}", file);
			}
		}
	}

	if ((node = npcNode.child("defenses"))) {
		if ((attr = node.attribute("defense"))) {
			npcType->info.defense = pugi::cast<int32_t>(attr.value());
		}

		if ((attr = node.attribute("armor"))) {
			npcType->info.armor = pugi::cast<int32_t>(attr.value());
		}

		for (auto defenseNode : node.children()) {
			spellBlockNpc_t sb;
			if (deserializeSpell(defenseNode, sb, npcName)) {
				npcType->info.defenseSpells.emplace_back(std::move(sb));
			} else {
				SPDLOG_WARN("[Npcs::loadNpc] - "
                            "Cant load spell. {}", file);
			}
		}
	}

	if ((node = npcNode.child("voices"))) {
		if ((attr = node.attribute("speed")) || (attr = node.attribute("interval"))) {
			npcType->info.yellSpeedTicks = pugi::cast<uint32_t>(attr.value());
		} else {
			SPDLOG_WARN("[Npcs::loadNpc] - "
                        "Missing voices speed. {}", file);
		}

		if ((attr = node.attribute("chance"))) {
			npcType->info.yellChance = pugi::cast<uint32_t>(attr.value());
		} else {
			SPDLOG_WARN("[Npcs::loadNpc] - "
                        "Missing voices chance. {}", file);
		}

		for (auto voiceNode : node.children()) {
			voiceBlock_t vb;
			if ((attr = voiceNode.attribute("sentence"))) {
				vb.text = attr.as_string();
			} else {
				SPDLOG_WARN("[Npcs::loadNpc] - "
                            "Missing voice sentence. {}", file);
			}

			if ((attr = voiceNode.attribute("yell"))) {
				vb.yellText = attr.as_bool();
			} else {
				vb.yellText = false;
			}
			npcType->info.voiceVector.emplace_back(vb);
		}
	}

	if ((node = npcNode.child("script"))) {
		for (auto eventNode : node.children()) {
			if ((attr = eventNode.attribute("name"))) {
				npcType->info.scripts.emplace_back(attr.as_string());
			} else {
				SPDLOG_WARN("[Npcs::loadNpc] - "
                            "Missing name for script event. {}", file);
			}
		}
	}

	npcType->info.attackSpells.shrink_to_fit();
	npcType->info.defenseSpells.shrink_to_fit();
	npcType->info.voiceVector.shrink_to_fit();
	npcType->info.scripts.shrink_to_fit();
	return npcType;
}

bool NpcType::loadCallback(LuaScriptInterface* scriptInterface)
{
	int32_t id = scriptInterface->getEvent();
	if (id == -1) {
		SPDLOG_WARN("[NpcType::loadCallback] - Event not found");
		return false;
	}

	info.scriptInterface = scriptInterface;
	if (info.eventType == NPCS_EVENT_THINK) {
		info.thinkEvent = id;
	} else if (info.eventType == NPCS_EVENT_APPEAR) {
		info.creatureAppearEvent = id;
	} else if (info.eventType == NPCS_EVENT_DISAPPEAR) {
		info.creatureDisappearEvent = id;
	} else if (info.eventType == NPCS_EVENT_MOVE) {
		info.creatureMoveEvent = id;
	} else if (info.eventType == NPCS_EVENT_SAY) {
		info.creatureSayEvent = id;
	}
	return true;
}

NpcType* Npcs::getNpcType(const std::string& name)
{
	std::string lowerCaseName = asLowerCaseString(name);

	auto it = npcs.find(lowerCaseName);
	if (it == npcs.end()) {
		auto it2 = unloadedNpcs.find(lowerCaseName);
		if (it2 == unloadedNpcs.end()) {
			return nullptr;
		}

		return loadNpc(it2->second, name);
	}
	return &it->second;
}

void Npcs::addNpcType(const std::string& name, NpcType* npcType)
{
	// Suppress [-Werror=unused-but-set-parameter]
	// https://stackoverflow.com/questions/1486904/how-do-i-best-silence-a-warning-about-unused-variables
	(void) npcType;
	npcType = &npcs[asLowerCaseString(name)];
}
