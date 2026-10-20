/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/// AC's object state, as the 3.4.3 client's update fields.
///
/// AC keeps its values array (3.3.5's descriptor layout, UpdateFields.h) because the game logic
/// reads and writes it everywhere; each entity's FillUpdateFields maps it onto the 3.4.3 structs
/// (UpdateFieldsData.h, generated from the client), and PatchUpdateFields applies what depends on
/// the viewer. A value the server has no source for yet is left at its default with a TODO.

#include "Bag.h"
#include "Corpse.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DynamicObject.h"
#include "GameObject.h"
#include "Group.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PacketGuid128.h"
#include "Pet.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Transport.h"
#include "UpdateFieldsWriter.h"
#include "World.h"

namespace
{
    /// AC's (3.3.5) unit dynamic flags as the client's. 3.4.3 [TrinityCore 3.4.3 UnitDynFlags]: HIDE_MODEL
    /// 0x02, LOOTABLE 0x04, TRACK_UNIT 0x08, TAPPED 0x10 (tapped by someone else), SPECIALINFO 0x20,
    /// CAN_SKIN 0x40, REFER_A_FRIEND 0x80; 3.3.5's DEAD and TAPPED_BY_ALL_THREAT_LIST have no counterpart.
    uint32 ClientUnitDynamicFlags(uint32 flags, bool tappedByOther)
    {
        uint32 out = 0;
        if (flags & UNIT_DYNFLAG_LOOTABLE)
            out |= 0x04;
        if (flags & UNIT_DYNFLAG_TRACK_UNIT)
            out |= 0x08;
        if (tappedByOther)
            out |= 0x10;
        if (flags & UNIT_DYNFLAG_SPECIALINFO)
            out |= 0x20;
        if (flags & UNIT_DYNFLAG_REFER_A_FRIEND)
            out |= 0x80;
        return out;
    }

    /// AC's gameobject dynamic flags (low 16 bits) as the client's [TrinityCore 3.4.3
    /// GameObjectDynamicLowFlags]: ACTIVATE 0x04, ANIMATE 0x08, SPARKLE 0x20, STOPPED 0x40, NO_INTERACT 0x80.
    uint32 ClientGameObjectDynamicFlags(uint32 flags)
    {
        uint32 out = 0;
        if (flags & GO_DYNFLAG_LO_ACTIVATE)
            out |= 0x04;
        if (flags & GO_DYNFLAG_LO_ANIMATE)
            out |= 0x08;
        if (flags & GO_DYNFLAG_LO_NO_INTERACT)
            out |= 0x80;
        if (flags & GO_DYNFLAG_LO_SPARKLE)
            out |= 0x20;
        if (flags & GO_DYNFLAG_LO_STOPPED)
            out |= 0x40;
        return out;
    }

    /// AC's player flags as the client's: 3.4.3 reuses 0x400/0x800 (HIDE_HELM/HIDE_CLOAK in 3.3.5) for
    /// war mode and 0x800000 (ALLOW_ONLY_ABILITY) for HIDE_ACCOUNT_ACHIEVEMENTS; those are dropped.
    /// TODO: 3.4.3's helm/cloak display options.
    uint32 ClientPlayerFlags(uint32 flags)
    {
        return flags & ~uint32(0x00000400 | 0x00000800 | 0x00800000);
    }

    /// The 3.4.3 power types (PowerType.db2): AC's 3.3.5 ones keep their numbers, plus combo points
    /// (14) and the death knight's three rune types (20 blood, 21 frost, 22 unholy).
    constexpr uint8 CLIENT_POWER_COMBO_POINTS = 14;
    constexpr uint8 CLIENT_POWER_RUNE_BLOOD = 20;
    constexpr uint8 CLIENT_POWER_RUNE_FROST = 21;
    constexpr uint8 CLIENT_POWER_RUNE_UNHOLY = 22;

    /// A class's power types, ascending: its UnitData.Power[] slots (ChrClassesXPowerTypes.db2, which
    /// the client orders by class and power type to index a unit's powers).
    std::vector<uint8> const& ClassPowerTypes(uint8 classId)
    {
        static std::vector<uint8> const none = { POWER_MANA };
        static std::unordered_map<uint8, std::vector<uint8>> const powers =
        {
            { CLASS_WARRIOR,      { POWER_RAGE, CLIENT_POWER_COMBO_POINTS } },
            { CLASS_PALADIN,      { POWER_MANA } },
            { CLASS_HUNTER,       { POWER_MANA } },
            { CLASS_ROGUE,        { POWER_ENERGY, CLIENT_POWER_COMBO_POINTS } },
            { CLASS_PRIEST,       { POWER_MANA } },
            { CLASS_DEATH_KNIGHT, { POWER_RUNIC_POWER, CLIENT_POWER_RUNE_BLOOD, CLIENT_POWER_RUNE_FROST, CLIENT_POWER_RUNE_UNHOLY } },
            { CLASS_SHAMAN,       { POWER_MANA } },
            { CLASS_MAGE,         { POWER_MANA } },
            { CLASS_WARLOCK,      { POWER_MANA } },
            { CLASS_DRUID,        { POWER_MANA, POWER_RAGE, POWER_ENERGY, CLIENT_POWER_COMBO_POINTS } },
        };
        auto itr = powers.find(classId);
        return itr != powers.end() ? itr->second : none;
    }

    /// AC's inventory slots (3.3.5's layout) as the 3.4.3 client's InvSlots indices: equipment 0-18,
    /// bags 30-33, backpack 35-, bank 59-86, bank bags 87-93, buyback 94-105, keyring 106-137
    /// (Lua BankButtonIDToInvSlotID 0x3b / 0x57, KeyRingButtonIDToInvSlotID 0x6a, container ids
    /// FUN_141c5df80 0x1e / 0x53). 3.3.5's currency token slots have no place: -1.
    int32 ClientInventorySlot(uint8 slot)
    {
        if (slot < EQUIPMENT_SLOT_END)
            return slot;
        if (slot >= INVENTORY_SLOT_BAG_START && slot < INVENTORY_SLOT_BAG_END)
            return 30 + (slot - INVENTORY_SLOT_BAG_START);
        if (slot >= INVENTORY_SLOT_ITEM_START && slot < INVENTORY_SLOT_ITEM_END)
            return 35 + (slot - INVENTORY_SLOT_ITEM_START);
        if (slot >= BANK_SLOT_ITEM_START && slot < BANK_SLOT_ITEM_END)
            return 59 + (slot - BANK_SLOT_ITEM_START);
        if (slot >= BANK_SLOT_BAG_START && slot < BANK_SLOT_BAG_END)
            return 87 + (slot - BANK_SLOT_BAG_START);
        if (slot >= BUYBACK_SLOT_START && slot < BUYBACK_SLOT_END)
            return 94 + (slot - BUYBACK_SLOT_START);
        if (slot >= KEYRING_SLOT_START && slot < KEYRING_SLOT_END)
            return 106 + (slot - KEYRING_SLOT_START);
        return -1;
    }

    /// The GUID types of the account GUIDs a player carries [TrinityCore: HighGuid WowAccount 29, BNetAccount 30].
    constexpr uint64 WOW_ACCOUNT_HIGH = uint64(29) << 58;
    constexpr uint64 BNET_ACCOUNT_HIGH = uint64(30) << 58;
    /// A guild's GUID [TrinityCore: HighGuid Guild 28, realm at bit 42, the guild id low].
    constexpr uint64 GUILD_HIGH_TYPE = uint64(28) << 58;

    void SetGuid128(UF::Data& data, UF::UFField const& f, uint64 low, uint64 high, uint32 i = 0)
    {
        data.SetGuid128(f, low, low ? high : 0, i);
    }

    void SetGuildGuid(UF::Data& data, UF::UFField const& f, uint32 guildId)
    {
        SetGuid128(data, f, guildId, GUILD_HIGH_TYPE | Guid128::RealmBits());
    }

    void FillCustomizations(UF::Data& data, uint8 dyn, UF::UFField const& option, UF::UFField const& choice,
        std::vector<ChrCustomizationChoice> const& customizations)
    {
        data.DynClear(dyn);
        for (ChrCustomizationChoice const& c : customizations)
        {
            uint8* elem = data.DynAppend(dyn);
            UF::Data::PutElem(elem, option, c.ChrCustomizationOptionID);
            UF::Data::PutElem(elem, choice, c.ChrCustomizationChoiceID);
        }
    }
}

// ---- Item, Bag -------------------------------------------------------------------------------

void Item::FillUpdateFields(UF::Values& values) const
{
    Object::FillUpdateFields(values);
    using namespace UF::ItemFields;

    UF::Data& d = values.Add(UF::STRUCT_ITEM);
    d.SetGuid(Owner, GetGuidValue(ITEM_FIELD_OWNER));
    d.SetGuid(ContainedIn, GetGuidValue(ITEM_FIELD_CONTAINED));
    d.SetGuid(Creator, GetGuidValue(ITEM_FIELD_CREATOR));
    d.SetGuid(GiftCreator, GetGuidValue(ITEM_FIELD_GIFTCREATOR));
    d.Set(StackCount, GetUInt32Value(ITEM_FIELD_STACK_COUNT));
    d.Set(Expiration, GetUInt32Value(ITEM_FIELD_DURATION));
    for (uint32 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        d.Set(SpellCharges, GetUInt32Value(ITEM_FIELD_SPELL_CHARGES + i), i);
    d.Set(DynamicFlags, GetUInt32Value(ITEM_FIELD_FLAGS));          // TODO: audit ItemFieldFlags against 3.4.3
    for (uint32 slot = 0; slot < MAX_ENCHANTMENT_SLOT && slot < Enchantment_ID.Count; ++slot)
    {
        EnchantmentSlot s = EnchantmentSlot(slot);
        d.Set(Enchantment_ID, GetEnchantmentId(s), slot);
        d.Set(Enchantment_Duration, GetEnchantmentDuration(s), slot);
        d.Set(Enchantment_Charges, GetEnchantmentCharges(s), slot);
    }
    d.Set(PropertySeed, GetUInt32Value(ITEM_FIELD_PROPERTY_SEED));
    d.Set(RandomPropertiesID, GetUInt32Value(ITEM_FIELD_RANDOM_PROPERTIES_ID));
    d.Set(Durability, GetUInt32Value(ITEM_FIELD_DURABILITY));
    d.Set(MaxDurability, GetUInt32Value(ITEM_FIELD_MAXDURABILITY));
    d.Set(CreatePlayedTime, GetUInt32Value(ITEM_FIELD_CREATE_PLAYED_TIME));
    d.Set(ItemBonusKey_ItemID, GetEntry());
    // Context, CreateTime, ArtifactXP, ItemAppearanceModID, DynamicFlags2, gems: none in Wrath (0)
}

void Bag::FillUpdateFields(UF::Values& values) const
{
    Item::FillUpdateFields(values);
    using namespace UF::ContainerFields;

    UF::Data& d = values.Add(UF::STRUCT_CONTAINER);
    d.Set(NumSlots, GetBagSize());
    for (uint32 i = 0; i < Slots.Count && i < MAX_BAG_SIZE; ++i)
        d.SetGuid(Slots, GetGuidValue(CONTAINER_FIELD_SLOT_1 + i * 2), i);
}

// ---- Unit ------------------------------------------------------------------------------------

void Unit::FillUpdateFields(UF::Values& values) const
{
    Object::FillUpdateFields(values);
    using namespace UF::UnitFields;

    // the viewer-independent part; tapping and per-viewer masks are in PatchUpdateFields
    values.Get(UF::STRUCT_OBJECT)->Set(UF::ObjectFields::DynamicFlags,
        ClientUnitDynamicFlags(GetUInt32Value(UNIT_DYNAMIC_FLAGS), false));

    UF::Data& d = values.Add(UF::STRUCT_UNIT);
    d.Set(Health, GetUInt32Value(UNIT_FIELD_HEALTH));
    d.Set(MaxHealth, GetUInt32Value(UNIT_FIELD_MAXHEALTH));
    d.Set(DisplayID, GetUInt32Value(UNIT_FIELD_DISPLAYID));
    d.Set(NpcFlags, GetUInt32Value(UNIT_NPC_FLAGS), 0);
    d.SetGuid(Charm, GetGuidValue(UNIT_FIELD_CHARM));
    d.SetGuid(Summon, GetGuidValue(UNIT_FIELD_SUMMON));
    d.SetGuid(Critter, GetGuidValue(UNIT_FIELD_CRITTER));
    d.SetGuid(CharmedBy, GetGuidValue(UNIT_FIELD_CHARMEDBY));
    d.SetGuid(SummonedBy, GetGuidValue(UNIT_FIELD_SUMMONEDBY));
    d.SetGuid(CreatedBy, GetGuidValue(UNIT_FIELD_CREATEDBY));
    d.SetGuid(Target, GetGuidValue(UNIT_FIELD_TARGET));
    d.Set(ChannelData_SpellID, GetUInt32Value(UNIT_CHANNEL_SPELL));
    // TODO: ChannelData.SpellXSpellVisualID (SpellXSpellVisual.db2), without it the channel shows no visual
    if (ObjectGuid channelObject = GetGuidValue(UNIT_FIELD_CHANNEL_OBJECT))
    {
        uint8* elem = d.DynAppend(DynChannelObjects);
        auto [low, high] = Guid128::Encode(channelObject);
        UF::Data::Put(elem + ChannelObjects_ChannelObject.Offset, 8, low);
        UF::Data::Put(elem + ChannelObjects_ChannelObject.Offset + 8, 8, high);
    }

    d.Set(Race, getRace());
    d.Set(ClassId, getClass());
    d.Set(PlayerClassId, IsPlayer() ? getClass() : 0);
    d.Set(Sex, getGender());
    d.Set(DisplayPower, getPowerType());

    // powers: a player's by its class's power types, a creature's display power in slot 0
    if (Player const* player = ToPlayer())
    {
        std::vector<uint8> const& types = ClassPowerTypes(player->getClass());
        for (uint32 idx = 0; idx < types.size() && idx < Power.Count; ++idx)
        {
            uint8 type = types[idx];
            if (type < MAX_POWERS)
            {
                d.Set(Power, GetPower(Powers(type)), idx);
                d.Set(MaxPower, GetMaxPower(Powers(type)), idx);
                d.SetFloat(PowerRegenFlatModifier, GetFloatValue(UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER + type), idx);
                d.SetFloat(PowerRegenInterruptedFlatModifier, GetFloatValue(UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER + type), idx);
            }
            else if (type == CLIENT_POWER_COMBO_POINTS)
            {
                d.Set(Power, GetComboPoints(), idx);
                d.Set(MaxPower, 5, idx);
            }
            else if (player->getClass() == CLASS_DEATH_KNIGHT)
            {
                // the runes of one base type that are ready (two of each)
                RuneType runeType = type == CLIENT_POWER_RUNE_BLOOD ? RUNE_BLOOD : type == CLIENT_POWER_RUNE_FROST ? RUNE_FROST : RUNE_UNHOLY;
                uint32 ready = 0;
                for (uint8 i = 0; i < MAX_RUNES; ++i)
                    if (player->GetBaseRune(i) == runeType && !player->GetRuneCooldown(i))
                        ++ready;
                d.Set(Power, ready, idx);
                d.Set(MaxPower, 2, idx);
            }
        }
    }
    else
    {
        Powers power = getPowerType();
        if (power < MAX_POWERS)
        {
            d.Set(Power, GetPower(power), 0);
            d.Set(MaxPower, GetMaxPower(power), 0);
            d.SetFloat(PowerRegenFlatModifier, GetFloatValue(UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER + power), 0);
            d.SetFloat(PowerRegenInterruptedFlatModifier, GetFloatValue(UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER + power), 0);
        }
    }

    d.Set(Level, GetUInt32Value(UNIT_FIELD_LEVEL));
    d.Set(FactionTemplate, GetUInt32Value(UNIT_FIELD_FACTIONTEMPLATE));
    for (uint32 i = 0; i < VirtualItems_ItemID.Count; ++i)
        d.Set(VirtualItems_ItemID, GetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID + i), i);
    d.Set(Flags, GetUInt32Value(UNIT_FIELD_FLAGS));
    d.Set(Flags2, GetUInt32Value(UNIT_FIELD_FLAGS_2));                // TODO: audit UnitFlags2 against 3.4.3
    d.Set(AuraState, GetUInt32Value(UNIT_FIELD_AURASTATE));
    d.Set(AttackRoundBaseTime, uint32(std::max(0.0f, GetFloatValue(UNIT_FIELD_BASEATTACKTIME))), 0);
    d.Set(AttackRoundBaseTime, uint32(std::max(0.0f, GetFloatValue(UNIT_FIELD_BASEATTACKTIME + 1))), 1);
    d.Set(RangedAttackRoundBaseTime, uint32(std::max(0.0f, GetFloatValue(UNIT_FIELD_RANGEDATTACKTIME))));
    d.SetFloat(BoundingRadius, GetFloatValue(UNIT_FIELD_BOUNDINGRADIUS));
    d.SetFloat(CombatReach, GetFloatValue(UNIT_FIELD_COMBATREACH));
    d.SetFloat(DisplayScale, 1.0f);
    d.Set(NativeDisplayID, GetUInt32Value(UNIT_FIELD_NATIVEDISPLAYID));
    d.SetFloat(NativeXDisplayScale, 1.0f);
    d.Set(MountDisplayID, GetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID));
    d.SetFloat(MinDamage, GetFloatValue(UNIT_FIELD_MINDAMAGE));
    d.SetFloat(MaxDamage, GetFloatValue(UNIT_FIELD_MAXDAMAGE));
    d.SetFloat(MinOffHandDamage, GetFloatValue(UNIT_FIELD_MINOFFHANDDAMAGE));
    d.SetFloat(MaxOffHandDamage, GetFloatValue(UNIT_FIELD_MAXOFFHANDDAMAGE));

    d.Set(StandState, GetByteValue(UNIT_FIELD_BYTES_1, UNIT_BYTES_1_OFFSET_STAND_STATE));
    d.Set(PetTalentPoints, GetByteValue(UNIT_FIELD_BYTES_1, UNIT_BYTES_1_OFFSET_PET_TALENTS));
    d.Set(VisFlags, GetByteValue(UNIT_FIELD_BYTES_1, UNIT_BYTES_1_OFFSET_VIS_FLAG));
    // 3.3.5's animation flags byte; 3.4.3 has an anim tier (0 ground, 2 hover, 3 fly)
    d.Set(UF::UnitFields::AnimTier, (GetByteValue(UNIT_FIELD_BYTES_1, UNIT_BYTES_1_OFFSET_ANIM_TIER) & UNIT_BYTE1_FLAG_HOVER) ? 2 : 0);

    d.Set(PetNumber, GetUInt32Value(UNIT_FIELD_PETNUMBER));
    d.Set(PetNameTimestamp, GetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP));
    d.Set(PetExperience, GetUInt32Value(UNIT_FIELD_PETEXPERIENCE));
    d.Set(PetNextLevelExperience, GetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP));
    d.SetFloat(ModCastingSpeed, GetFloatValue(UNIT_MOD_CAST_SPEED));
    d.SetFloat(ModSpellHaste, GetFloatValue(UNIT_MOD_CAST_SPEED));
    d.SetFloat(ModHaste, 1.0f);                                       // TODO: melee haste
    d.SetFloat(ModRangedHaste, 1.0f);                                 // TODO: ranged haste
    d.SetFloat(ModHasteRegen, 1.0f);
    d.SetFloat(ModTimeRate, 1.0f);
    d.Set(CreatedBySpell, GetUInt32Value(UNIT_CREATED_BY_SPELL));
    d.Set(EmoteState, GetUInt32Value(UNIT_NPC_EMOTESTATE));

    for (uint32 i = 0; i < MAX_STATS; ++i)
    {
        d.Set(UF::UnitFields::Stats, GetUInt32Value(UNIT_FIELD_STAT0 + i), i);
        d.Set(StatPosBuff, uint32(int32(GetFloatValue(UNIT_FIELD_POSSTAT0 + i))), i);
        d.Set(StatNegBuff, uint32(int32(GetFloatValue(UNIT_FIELD_NEGSTAT0 + i))), i);
    }
    for (uint32 i = 0; i < MAX_SPELL_SCHOOL; ++i)
    {
        d.Set(Resistances, GetUInt32Value(UNIT_FIELD_RESISTANCES + i), i);
        d.Set(PowerCostModifier, GetUInt32Value(UNIT_FIELD_POWER_COST_MODIFIER + i), i);
        d.SetFloat(PowerCostMultiplier, GetFloatValue(UNIT_FIELD_POWER_COST_MULTIPLIER + i), i);
        d.Set(ResistanceBuffModsPositive, uint32(int32(GetFloatValue(UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE + i))), i);
        d.Set(ResistanceBuffModsNegative, uint32(int32(GetFloatValue(UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE + i))), i);
    }
    d.Set(BaseMana, GetUInt32Value(UNIT_FIELD_BASE_MANA));
    d.Set(BaseHealth, GetUInt32Value(UNIT_FIELD_BASE_HEALTH));
    d.Set(SheatheState, GetByteValue(UNIT_FIELD_BYTES_2, 0));
    d.Set(PvpFlags, GetByteValue(UNIT_FIELD_BYTES_2, 1));
    d.Set(PetFlags, GetByteValue(UNIT_FIELD_BYTES_2, 2));
    d.Set(UF::UnitFields::ShapeshiftForm, GetByteValue(UNIT_FIELD_BYTES_2, 3));
    d.Set(AttackPower, GetUInt32Value(UNIT_FIELD_ATTACK_POWER));
    d.Set(AttackPowerModPos, uint32(int32(int16(GetUInt16Value(UNIT_FIELD_ATTACK_POWER_MODS, 0)))));
    d.Set(AttackPowerModNeg, uint32(int32(int16(GetUInt16Value(UNIT_FIELD_ATTACK_POWER_MODS, 1)))));
    d.SetFloat(AttackPowerMultiplier, GetFloatValue(UNIT_FIELD_ATTACK_POWER_MULTIPLIER));
    d.Set(RangedAttackPower, GetUInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER));
    d.Set(RangedAttackPowerModPos, uint32(int32(int16(GetUInt16Value(UNIT_FIELD_RANGED_ATTACK_POWER_MODS, 0)))));
    d.Set(RangedAttackPowerModNeg, uint32(int32(int16(GetUInt16Value(UNIT_FIELD_RANGED_ATTACK_POWER_MODS, 1)))));
    d.SetFloat(RangedAttackPowerMultiplier, GetFloatValue(UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER));
    d.SetFloat(MinRangedDamage, GetFloatValue(UNIT_FIELD_MINRANGEDDAMAGE));
    d.SetFloat(MaxRangedDamage, GetFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE));
    d.SetFloat(MaxHealthModifier, GetFloatValue(UNIT_FIELD_MAXHEALTHMODIFIER));
    d.SetFloat(HoverHeight, GetFloatValue(UNIT_FIELD_HOVERHEIGHT));

    if (Player const* player = ToPlayer())
    {
        if (uint32 guildId = player->GetGuildId())
            SetGuildGuid(d, GuildGUID, guildId);
        d.Set(CurrentAreaID, player->GetAreaId());
        d.SetGuid(ComboTarget, GetComboTargetGUID());
    }
}

void Unit::PatchUpdateFields(UF::Values& values, Player const* target) const
{
    using namespace UF::UnitFields;
    UF::Data& d = *values.Get(UF::STRUCT_UNIT);
    Creature const* creature = ToCreature();
    Player* viewer = const_cast<Player*>(target);

    // UNIT_NPC_FLAGS: what this viewer may use
    if (creature)
    {
        uint32 npcFlags = GetUInt32Value(UNIT_NPC_FLAGS);
        if (sWorld->getIntConfig(CONFIG_INSTANT_TAXI) == 2 && npcFlags & UNIT_NPC_FLAG_FLIGHTMASTER)
            npcFlags |= UNIT_NPC_FLAG_GOSSIP;
        if (!target->CanSeeSpellClickOn(creature))
            npcFlags &= ~UNIT_NPC_FLAG_SPELLCLICK;
        if (!target->CanSeeVendor(creature))
            npcFlags &= ~(UNIT_NPC_FLAG_REPAIR | UNIT_NPC_FLAG_VENDOR_MASK);
        if (!target->CanSeeTrainer(creature))
            npcFlags &= ~UNIT_NPC_FLAG_TRAINER;
        d.Set(NpcFlags, npcFlags, 0);
    }

    // UNIT_FIELD_AURASTATE: per-caster aura states
    d.Set(AuraState, BuildAuraStateUpdateForTarget(viewer));

    // UNIT_FIELD_FLAGS: game masters can select anything
    uint32 flags = GetUInt32Value(UNIT_FIELD_FLAGS);
    if (target->IsGameMaster() && target->GetSession()->IsGMAccount())
        flags &= ~UNIT_FLAG_NOT_SELECTABLE;
    d.Set(Flags, flags);

    // UNIT_FIELD_DISPLAYID: trigger creatures are invisible to all but game masters
    if (creature)
    {
        CreatureTemplate const* cinfo = creature->GetCreatureTemplate();
        if (SpellInfo const* transform = sSpellMgr->GetSpellInfo(getTransForm()))
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (transform->Effects[i].IsAura(SPELL_AURA_TRANSFORM))
                    if (CreatureTemplate const* transformInfo = sObjectMgr->GetCreatureTemplate(transform->Effects[i].MiscValue))
                    {
                        cinfo = transformInfo;
                        break;
                    }

        if (cinfo->HasFlagsExtra(CREATURE_FLAG_EXTRA_TRIGGER))
        {
            if (target->IsGameMaster() && target->GetSession()->IsGMAccount())
                d.Set(DisplayID, cinfo->GetFirstVisibleModel()->CreatureDisplayID);
            else
                d.Set(DisplayID, cinfo->GetFirstInvisibleModel()->CreatureDisplayID);
        }
    }

    // UNIT_DYNAMIC_FLAGS: tapping and lootability are per viewer
    uint32 dynamicFlags = GetUInt32Value(UNIT_DYNAMIC_FLAGS) & ~(UNIT_DYNFLAG_TAPPED | UNIT_DYNFLAG_TAPPED_BY_PLAYER);
    bool tappedByOther = false;
    if (creature)
    {
        if (creature->hasLootRecipient() && !creature->isTappedBy(target))
            tappedByOther = true;
        if (!viewer->isAllowedToLoot(creature))
            dynamicFlags &= ~UNIT_DYNFLAG_LOOTABLE;
    }
    if (dynamicFlags & UNIT_DYNFLAG_TRACK_UNIT)
        if (!HasAuraTypeWithCaster(SPELL_AURA_MOD_STALKED, target->GetGUID()))
            dynamicFlags &= ~UNIT_DYNFLAG_TRACK_UNIT;
    values.Get(UF::STRUCT_OBJECT)->Set(UF::ObjectFields::DynamicFlags, ClientUnitDynamicFlags(dynamicFlags, tappedByOther));

    // UNIT_FIELD_BYTES_2 / UNIT_FIELD_FACTIONTEMPLATE: cross-faction groups and arena spectators
    if (IsControlledByPlayer() && target != this && sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GROUP) && IsInRaidWith(target))
    {
        FactionTemplateEntry const* ft1 = GetFactionTemplateEntry();
        FactionTemplateEntry const* ft2 = target->GetFactionTemplateEntry();
        if (ft1 && ft2 && !ft1->IsFriendlyTo(*ft2))
        {
            d.Set(PvpFlags, GetByteValue(UNIT_FIELD_BYTES_2, 1) & UNIT_BYTE2_FLAG_SANCTUARY);
            d.Set(FactionTemplate, target->GetFaction());
        }
    }
    else if (target->IsSpectator() && target->FindMap() && target->FindMap()->IsBattleArena() && (IsPlayer() || IsCreature()))
    {
        d.Set(PvpFlags, GetByteValue(UNIT_FIELD_BYTES_2, 1) & ~(UNIT_BYTE2_FLAG_PVP | UNIT_BYTE2_FLAG_FFA_PVP | UNIT_BYTE2_FLAG_SANCTUARY));
        d.Set(FactionTemplate, target->GetFaction());
    }
    else if (target->IsGMSpectator() && IsControlledByPlayer())
        d.Set(FactionTemplate, target->GetFaction());
}

// ---- Player ----------------------------------------------------------------------------------

void Player::FillUpdateFields(UF::Values& values) const
{
    Unit::FillUpdateFields(values);

    {
        using namespace UF::PlayerFields;
        UF::Data& d = values.Add(UF::STRUCT_PLAYER);
        d.SetGuid(DuelArbiter, GetGuidValue(PLAYER_DUEL_ARBITER));
        SetGuid128(d, WowAccount, GetSession()->GetAccountId(), WOW_ACCOUNT_HIGH);
        SetGuid128(d, BnetAccount, GetSession()->GetAccountId(), BNET_ACCOUNT_HIGH);   // TODO: the Battle.net account id
        d.Set(UF::PlayerFields::PlayerFlags, ClientPlayerFlags(GetUInt32Value(PLAYER_FLAGS)));
        d.Set(GuildRankID, GetUInt32Value(PLAYER_GUILDRANK));
        FillCustomizations(d, DynCustomizations, Customizations_ChrCustomizationOptionID,
            Customizations_ChrCustomizationChoiceID, m_customizations);
        d.Set(NumBankSlots, GetByteValue(PLAYER_BYTES_2, PLAYER_BYTES_2_OFFSET_BANK_BAG_SLOTS));
        d.Set(NativeSex, GetByteValue(PLAYER_BYTES_3, PLAYER_BYTES_3_OFFSET_GENDER));
        d.Set(Inebriation, GetByteValue(PLAYER_BYTES_3, PLAYER_BYTES_3_OFFSET_INEBRIATION));
        d.Set(PvpTitle, GetByteValue(PLAYER_BYTES_3, PLAYER_BYTES_3_OFFSET_PVP_TITLE));
        d.Set(ArenaFaction, GetByteValue(PLAYER_BYTES_3, PLAYER_BYTES_3_OFFSET_ARENA_FACTION));
        d.Set(DuelTeam, GetUInt32Value(PLAYER_DUEL_TEAM));
        d.Set(GuildTimeStamp, GetUInt32Value(PLAYER_GUILD_TIMESTAMP));

        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE && slot < QuestLog_QuestID.Count; ++slot)
        {
            d.Set(QuestLog_QuestID, GetQuestSlotQuestId(slot), slot);
            d.Set(QuestLog_StateFlags, GetQuestSlotState(slot), slot);
            d.Set(QuestLog_EndTime, GetQuestSlotTime(slot), slot);
            for (uint8 counter = 0; counter < 4; ++counter)
                d.Set(QuestLog_ObjectiveProgress, GetQuestSlotCounter(slot, counter), slot, counter);
        }

        for (uint32 i = 0; i < VisibleItems_ItemID.Count && i < EQUIPMENT_SLOT_END; ++i)
        {
            d.Set(VisibleItems_ItemID, GetUInt32Value(PLAYER_VISIBLE_ITEM_1_ENTRYID + i * 2), i);
            if (uint32 enchant = GetUInt16Value(PLAYER_VISIBLE_ITEM_1_ENCHANTMENT + i * 2, PERM_ENCHANTMENT_SLOT))
                if (SpellItemEnchantmentEntry const* entry = sSpellItemEnchantmentStore.LookupEntry(enchant))
                    d.Set(VisibleItems_ItemVisual, entry->aura_id, i);
        }

        d.Set(PlayerTitle, GetUInt32Value(PLAYER_CHOSEN_TITLE));
        d.Set(FakeInebriation, GetUInt32Value(PLAYER_FAKE_INEBRIATION));
        d.Set(VirtualPlayerRealm, Guid128::VirtualRealmAddress());
        // TODO: AvgItemLevel, HonorLevel, LogoutTime: not tracked by AC
    }

    {
        using namespace UF::ActivePlayerFields;
        UF::Data& d = values.Add(UF::STRUCT_ACTIVE_PLAYER);

        for (uint8 slot = 0; slot < PLAYER_SLOT_END; ++slot)
        {
            int32 clientSlot = ClientInventorySlot(slot);
            if (clientSlot >= 0 && uint32(clientSlot) < InvSlots.Count)
                d.SetGuid(InvSlots, GetGuidValue(PLAYER_FIELD_INV_SLOT_HEAD + slot * 2), uint32(clientSlot));
        }

        d.SetGuid(FarsightObject, GetGuidValue(PLAYER_FARSIGHT));
        d.Set(Coinage, GetUInt32Value(PLAYER_FIELD_COINAGE));
        d.Set(XP, GetUInt32Value(PLAYER_XP));
        d.Set(NextLevelXP, GetUInt32Value(PLAYER_NEXT_LEVEL_XP));

        for (uint32 i = 0; i < PLAYER_MAX_SKILLS && i < SkillLineID.Count; ++i)
        {
            uint32 idAndStep = GetUInt32Value(PLAYER_SKILL_INDEX(i));
            uint32 rankAndMax = GetUInt32Value(PLAYER_SKILL_VALUE_INDEX(i));
            uint32 bonus = GetUInt32Value(PLAYER_SKILL_BONUS_INDEX(i));
            if (!idAndStep)
                continue;
            d.Set(SkillLineID, PAIR32_LOPART(idAndStep), i);
            d.Set(SkillStep, PAIR32_HIPART(idAndStep), i);
            d.Set(SkillRank, SKILL_VALUE(rankAndMax), i);
            d.Set(SkillStartingRank, 1, i);
            d.Set(SkillMaxRank, SKILL_MAX(rankAndMax), i);
            d.Set(SkillTempBonus, uint16(SKILL_TEMP_BONUS(bonus)), i);
            d.Set(SkillPermBonus, uint16(SKILL_PERM_BONUS(bonus)), i);
        }

        d.Set(CharacterPoints, GetUInt32Value(PLAYER_CHARACTER_POINTS1));
        d.Set(TrackCreatureMask, GetUInt32Value(PLAYER_TRACK_CREATURES));
        d.Set(TrackResourceMask, GetUInt32Value(PLAYER_TRACK_RESOURCES), 0);
        d.SetFloat(MainhandExpertise, float(GetInt32Value(PLAYER_EXPERTISE)));
        d.SetFloat(OffhandExpertise, float(GetInt32Value(PLAYER_OFFHAND_EXPERTISE)));
        d.SetFloat(BlockPercentage, GetFloatValue(PLAYER_BLOCK_PERCENTAGE));
        d.SetFloat(DodgePercentage, GetFloatValue(PLAYER_DODGE_PERCENTAGE));
        d.SetFloat(ParryPercentage, GetFloatValue(PLAYER_PARRY_PERCENTAGE));
        d.SetFloat(CritPercentage, GetFloatValue(PLAYER_CRIT_PERCENTAGE));
        d.SetFloat(RangedCritPercentage, GetFloatValue(PLAYER_RANGED_CRIT_PERCENTAGE));
        d.SetFloat(OffhandCritPercentage, GetFloatValue(PLAYER_OFFHAND_CRIT_PERCENTAGE));
        for (uint32 i = 0; i < MAX_SPELL_SCHOOL; ++i)
        {
            d.SetFloat(SpellCritPercentage, GetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1 + i), i);
            d.Set(ModDamageDonePos, GetUInt32Value(PLAYER_FIELD_MOD_DAMAGE_DONE_POS + i), i);
            d.Set(ModDamageDoneNeg, GetUInt32Value(PLAYER_FIELD_MOD_DAMAGE_DONE_NEG + i), i);
            d.SetFloat(ModDamageDonePercent, GetFloatValue(PLAYER_FIELD_MOD_DAMAGE_DONE_PCT + i), i);
        }
        d.Set(ShieldBlock, GetUInt32Value(PLAYER_SHIELD_BLOCK));
        d.SetFloat(ShieldBlockCritPercentage, GetFloatValue(PLAYER_SHIELD_BLOCK_CRIT_PERCENTAGE));

        // explored areas: AreaTable's bit numbers in both; 3.3.5's 128 words are the first 64 of 240
        for (uint32 i = 0; i < PLAYER_EXPLORED_ZONES_SIZE && i / 2 < ExploredZones.Count; ++i)
            d.Set(ExploredZones, d.Get(ExploredZones, i / 2) | (uint64(GetUInt32Value(PLAYER_EXPLORED_ZONES_1 + i)) << (32 * (i % 2))), i / 2);

        d.Set(RestInfo_Threshold, GetUInt32Value(PLAYER_REST_STATE_EXPERIENCE), 0);
        d.Set(RestInfo_StateID, GetByteValue(PLAYER_BYTES_2, PLAYER_BYTES_2_OFFSET_REST_STATE), 0);
        d.Set(ModHealingDonePos, GetUInt32Value(PLAYER_FIELD_MOD_HEALING_DONE_POS));
        d.SetFloat(ModHealingPercent, GetFloatValue(PLAYER_FIELD_MOD_HEALING_PCT));
        d.SetFloat(ModHealingDonePercent, GetFloatValue(PLAYER_FIELD_MOD_HEALING_DONE_PCT));
        d.SetFloat(ModPeriodicHealingDonePercent, 1.0f);
        for (uint32 i = 0; i < WeaponDmgMultipliers.Count; ++i)
        {
            d.SetFloat(WeaponDmgMultipliers, 1.0f, i);
            d.SetFloat(WeaponAtkSpeedMultipliers, 1.0f, i);
        }
        d.SetFloat(ModSpellPowerPercent, 1.0f);
        d.Set(ModTargetResistance, GetUInt32Value(PLAYER_FIELD_MOD_TARGET_RESISTANCE));
        d.Set(ModTargetPhysicalResistance, GetUInt32Value(PLAYER_FIELD_MOD_TARGET_PHYSICAL_RESISTANCE));
        // PLAYER_FIELD_BYTES: local flags (same bits in 3.4.3's PlayerLocalFlags), RAF levels, action bars, max rank
        d.Set(UF::ActivePlayerFields::LocalFlags, GetByteValue(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTES_OFFSET_FLAGS));
        d.Set(GrantableLevels, GetByteValue(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTES_OFFSET_RAF_GRANTABLE_LEVEL));
        d.Set(MultiActionBars, GetByteValue(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTES_OFFSET_ACTION_BAR_TOGGLES));
        d.Set(LifetimeMaxRank, GetByteValue(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTES_OFFSET_LIFETIME_MAX_PVP_RANK));
        d.Set(AmmoID, GetUInt32Value(PLAYER_AMMO_ID));
        d.Set(PvpMedals, GetUInt32Value(PLAYER_FIELD_PVP_MEDALS));
        for (uint32 i = 0; i < BuybackPrice.Count && i < (BUYBACK_SLOT_END - BUYBACK_SLOT_START); ++i)
        {
            d.Set(BuybackPrice, GetUInt32Value(PLAYER_FIELD_BUYBACK_PRICE_1 + i), i);
            d.Set(BuybackTimestamp, GetUInt32Value(PLAYER_FIELD_BUYBACK_TIMESTAMP_1 + i), i);
        }
        d.Set(TodayHonorableKills, GetUInt16Value(PLAYER_FIELD_KILLS, 0));
        d.Set(YesterdayHonorableKills, GetUInt16Value(PLAYER_FIELD_KILLS, 1));
        d.Set(YesterdayContribution, GetUInt32Value(PLAYER_FIELD_YESTERDAY_CONTRIBUTION));
        d.Set(LifetimeHonorableKills, GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS));
        d.Set(WatchedFactionIndex, GetUInt32Value(PLAYER_FIELD_WATCHED_FACTION_INDEX));
        for (uint32 i = 0; i < MAX_COMBAT_RATING && i < CombatRatings.Count; ++i)
            d.Set(CombatRatings, GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + i), i);
        d.Set(MaxLevel, GetUInt32Value(PLAYER_FIELD_MAX_LEVEL));
        for (uint32 i = 0; i < 3 && i < NoReagentCostMask.Count; ++i)
            d.Set(NoReagentCostMask, GetUInt32Value(PLAYER_NO_REAGENT_COST_1 + i), i);
        d.Set(PetSpellPower, GetUInt32Value(PLAYER_PET_SPELL_POWER));
        // PLAYER_FIELD_BYTES2: override spells id (u16), regen prediction mask, aura vision
        d.Set(OverrideSpellsID, GetUInt16Value(PLAYER_FIELD_BYTES2, PLAYER_BYTES_2_OVERRIDE_SPELLS_UINT16_OFFSET));
        d.Set(LocalRegenFlags, GetByteValue(PLAYER_FIELD_BYTES2, PLAYER_FIELD_BYTES_2_OFFSET_IGNORE_POWER_REGEN_PREDICTION_MASK));
        d.Set(AuraVision, GetByteValue(PLAYER_FIELD_BYTES2, PLAYER_FIELD_BYTES_2_OFFSET_AURA_VISION));
        d.Set(NumBackpackSlots, INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START);
        for (uint32 i = 0; i < MAX_GLYPH_SLOT_INDEX && i < GlyphSlots.Count; ++i)
        {
            d.Set(GlyphSlots, GetUInt32Value(PLAYER_FIELD_GLYPH_SLOTS_1 + i), i);
            d.Set(Glyphs, GetUInt32Value(PLAYER_FIELD_GLYPHS_1 + i), i);
        }
        d.Set(GlyphsEnabled, GetUInt32Value(PLAYER_GLYPHS_ENABLED));

        // known titles: 3.3.5's three u64 words
        d.DynClear(DynKnownTitles);
        for (uint32 i = 0; i < 3; ++i)
            UF::Data::PutElem(d.DynAppend(DynKnownTitles), KnownTitles_KnownTitlesWord, GetUInt64Value(PLAYER__FIELD_KNOWN_TITLES + i * 2));

        d.DynClear(DynSelfResSpells);
        if (uint32 selfRes = GetUInt32Value(PLAYER_SELF_RES_SPELL))
            UF::Data::PutElem(d.DynAppend(DynSelfResSpells), SelfResSpells_Field_0, selfRes);

        // TODO: QuestCompleted (the rewarded quests' QuestV2 UniqueBitFlag), DailyQuestsCompleted,
        // ProfessionSkillLine, Honor: not mapped yet
    }
}

// ---- GameObject ------------------------------------------------------------------------------

void GameObject::FillUpdateFields(UF::Values& values) const
{
    Object::FillUpdateFields(values);
    using namespace UF::GameObjectFields;

    values.Get(UF::STRUCT_OBJECT)->Set(UF::ObjectFields::DynamicFlags, 0);   // per viewer (PatchUpdateFields)

    UF::Data& d = values.Add(UF::STRUCT_GAMEOBJECT);
    d.Set(DisplayID, GetUInt32Value(GAMEOBJECT_DISPLAYID));
    d.SetGuid(CreatedBy, GetGuidValue(OBJECT_FIELD_CREATED_BY));
    d.Set(Flags, GetUInt32Value(GAMEOBJECT_FLAGS));                   // TODO: audit GameObjectFlags against 3.4.3
    d.SetFloat(ParentRotation_x, GetFloatValue(GAMEOBJECT_PARENTROTATION + 0));
    d.SetFloat(ParentRotation_y, GetFloatValue(GAMEOBJECT_PARENTROTATION + 1));
    d.SetFloat(ParentRotation_z, GetFloatValue(GAMEOBJECT_PARENTROTATION + 2));
    d.SetFloat(ParentRotation_w, GetFloatValue(GAMEOBJECT_PARENTROTATION + 3));
    d.Set(FactionTemplate, GetUInt32Value(GAMEOBJECT_FACTION));
    d.Set(Level, GetUInt32Value(GAMEOBJECT_LEVEL));
    d.Set(State, GetByteValue(GAMEOBJECT_BYTES_1, 0));
    d.Set(UF::GameObjectFields::TypeID, GetByteValue(GAMEOBJECT_BYTES_1, 1));
    d.Set(ArtKit, GetByteValue(GAMEOBJECT_BYTES_1, 2));
    d.Set(PercentHealth, GetByteValue(GAMEOBJECT_BYTES_1, 3));
}

void GameObject::PatchUpdateFields(UF::Values& values, Player const* target) const
{
    Player* viewer = const_cast<Player*>(target);
    bool targetIsGM = target->IsGameMaster() && target->GetSession()->IsGMAccount();

    // GAMEOBJECT_DYNAMIC: quest activation and sparkles per viewer; transports' path progress in the high half
    uint32 dynFlags = 0;
    int16 pathProgress = -1;
    switch (GetGoType())
    {
        case GAMEOBJECT_TYPE_QUESTGIVER:
            if (ActivateToQuest(viewer))
                dynFlags |= GO_DYNFLAG_LO_ACTIVATE;
            break;
        case GAMEOBJECT_TYPE_CHEST:
        case GAMEOBJECT_TYPE_GOOBER:
            if (ActivateToQuest(viewer))
            {
                dynFlags |= GO_DYNFLAG_LO_ACTIVATE;
                if (sWorld->getBoolConfig(CONFIG_OBJECT_SPARKLES))
                    dynFlags |= GO_DYNFLAG_LO_SPARKLE;
            }
            else if (targetIsGM)
                dynFlags |= GO_DYNFLAG_LO_ACTIVATE;
            break;
        case GAMEOBJECT_TYPE_SPELL_FOCUS:
        case GAMEOBJECT_TYPE_GENERIC:
            if (ActivateToQuest(viewer) && sWorld->getBoolConfig(CONFIG_OBJECT_SPARKLES))
                dynFlags |= GO_DYNFLAG_LO_SPARKLE;
            break;
        case GAMEOBJECT_TYPE_TRANSPORT:
            if (StaticTransport const* t = ToStaticTransport())
                if (t->GetPauseTime())
                {
                    if (GetGoState() == GO_STATE_READY)
                    {
                        if (t->GetPathProgress() >= t->GetPauseTime())
                            pathProgress = int16(float(t->GetPathProgress() - t->GetPauseTime()) / float(t->GetPeriod() - t->GetPauseTime()) * 65535.0f);
                    }
                    else if (t->GetPathProgress() <= t->GetPauseTime())
                        pathProgress = int16(float(t->GetPathProgress()) / float(t->GetPauseTime()) * 65535.0f);
                }
            break;
        case GAMEOBJECT_TYPE_MO_TRANSPORT:
            if (MotionTransport const* t = ToMotionTransport())
                pathProgress = int16(float(t->GetPathProgress()) / float(t->GetPeriod()) * 65535.0f);
            break;
        default:
            break;
    }
    values.Get(UF::STRUCT_OBJECT)->Set(UF::ObjectFields::DynamicFlags,
        ClientGameObjectDynamicFlags(dynFlags) | (uint32(uint16(pathProgress)) << 16));

    // GAMEOBJECT_FLAGS: group loot chests are locked to those who may not loot them
    uint32 goFlags = GetUInt32Value(GAMEOBJECT_FLAGS);
    if (GetGoType() == GAMEOBJECT_TYPE_CHEST && GetGOInfo() && GetGOInfo()->chest.groupLootRules && !IsLootAllowedFor(target))
        goFlags |= GO_FLAG_LOCKED | GO_FLAG_NOT_SELECTABLE;
    values.Get(UF::STRUCT_GAMEOBJECT)->Set(UF::GameObjectFields::Flags, goFlags);
}

// ---- DynamicObject ---------------------------------------------------------------------------

void DynamicObject::FillUpdateFields(UF::Values& values) const
{
    Object::FillUpdateFields(values);
    using namespace UF::DynamicObjectFields;

    UF::Data& d = values.Add(UF::STRUCT_DYNAMICOBJECT);
    d.SetGuid(Caster, GetCasterGUID());
    d.Set(Type, GetUInt32Value(DYNAMICOBJECT_BYTES) >> 28);
    // TODO: SpellVisual.SpellXSpellVisualID (3.3.5's bytes hold a SpellVisual id; 3.4.3 wants SpellXSpellVisual)
    d.Set(SpellID, GetSpellId());
    d.SetFloat(Radius, GetRadius());
    d.Set(CastTime, GetUInt32Value(DYNAMICOBJECT_CASTTIME));
}

// ---- Corpse ----------------------------------------------------------------------------------

void Corpse::FillUpdateFields(UF::Values& values) const
{
    Object::FillUpdateFields(values);
    using namespace UF::CorpseFields;

    UF::Data& d = values.Add(UF::STRUCT_CORPSE);
    d.Set(DynamicFlags, GetUInt32Value(CORPSE_FIELD_DYNAMIC_FLAGS));
    d.SetGuid(Owner, GetOwnerGUID());
    d.SetGuid(PartyGUID, GetGuidValue(CORPSE_FIELD_PARTY));
    if (uint32 guildId = GetUInt32Value(CORPSE_FIELD_GUILD))
        SetGuildGuid(d, GuildGUID, guildId);
    d.Set(DisplayID, GetUInt32Value(CORPSE_FIELD_DISPLAY_ID));
    for (uint32 i = 0; i < EQUIPMENT_SLOT_END && i < Items.Count; ++i)
        d.Set(Items, GetUInt32Value(CORPSE_FIELD_ITEM + i), i);
    d.Set(RaceID, GetByteValue(CORPSE_FIELD_BYTES_1, 1));
    d.Set(Sex, GetByteValue(CORPSE_FIELD_BYTES_1, 2));
    d.Set(Flags, GetUInt32Value(CORPSE_FIELD_FLAGS));
    // TODO: an offline owner's class and customizations (corpses keep only 3.3.5's appearance bytes)
    if (Player* owner = ObjectAccessor::FindConnectedPlayer(GetOwnerGUID()))
    {
        d.Set(Class, owner->getClass());
        FillCustomizations(d, DynCustomizations, Customizations_ChrCustomizationOptionID,
            Customizations_ChrCustomizationChoiceID, owner->GetCustomizations());
        d.Set(FactionTemplate, owner->GetFaction());
    }
}
