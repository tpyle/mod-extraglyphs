/*
 * mod-extraglyphs - glyph effects beyond the six sockets.
 *
 * Socketing a glyph does two things: it writes the glyph id into one of six
 * update fields the client draws its glyph UI from, and it casts
 * GlyphProperties.SpellId on the player as a passive aura. Only the second
 * part is the effect. The six is a client limit (PLAYER_FIELD_GLYPHS_1 is a
 * six-wide field in a fixed protocol layout shared with Wow.exe), so instead
 * of fighting it this module keeps its own list of "extra" glyphs per
 * character and per talent spec, and applies their spells the same way the
 * core applies socketed ones - CastSpell on login and spec switch,
 * RemoveAurasDueToSpell on removal.
 *
 * Nothing about it goes through the socket path. The glyph item's own use
 * spell (SPELL_EFFECT_APPLY_GLYPH) still targets a socket and is left alone;
 * extras are added by command, from the ExtraGlyphs addon panel
 * (client-patch/addon/ExtraGlyphs), over the addon command channel the core
 * already provides ("AzerothCore" prefix, opcode 'i', parsed by
 * AddonChannelCommandHandler::ParseCommands and run as a command from the
 * player):
 *
 *     extraglyphs list                active extras for the current spec
 *     extraglyphs catalog             every glyph the class can use
 *     extraglyphs add <glyphId>       apply one, consuming the glyph item
 *     extraglyphs remove <glyphId>    drop one; the glyph is lost, as with sockets
 *
 * Which glyphs a class may use comes from the glyph items themselves: every
 * item of class ITEM_CLASS_GLYPH carries a use spell whose APPLY_GLYPH effect
 * names the glyph id, and AllowableClass says who may use it. That map is
 * built once at startup, and it is also how "add" knows which item to consume.
 *
 * Storage is one row per (guid, spec, glyph) in acore_characters.
 * character_extra_glyphs, written through on every change and read once at
 * login. Rows go with the character on deletion.
 *
 * Security: SEC_PLAYER and Console::No; every command acts only on the caller.
 */

#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldSession.h"

#include <map>
#include <set>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    struct ExtraGlyphsConfig
    {
        bool   Enable      = true;
        uint32 Max         = 6;     // per spec; 0 = unlimited
        bool   RequireItem = true;
    };

    ExtraGlyphsConfig cfg;

    void LoadConfig()
    {
        cfg.Enable      = sConfigMgr->GetOption<bool>("ExtraGlyphs.Enable", true);
        cfg.Max         = sConfigMgr->GetOption<uint32>("ExtraGlyphs.Max", 6);
        cfg.RequireItem = sConfigMgr->GetOption<bool>("ExtraGlyphs.RequireItem", true);
    }

    // What a glyph is, as far as this module cares: the passive it grants,
    // the item that teaches it and who may use that item.
    struct GlyphDef
    {
        uint32 SpellId        = 0;
        uint32 TypeFlags      = 0;   // 0 major, 1 minor (GlyphProperties.TypeFlags)
        uint32 ItemEntry      = 0;
        uint32 AllowableClass = 0;
    };

    std::map<uint32 /*glyphId*/, GlyphDef> glyphDefs;

    void LoadGlyphDefs()
    {
        glyphDefs.clear();

        for (ItemTemplate const* proto : *sObjectMgr->GetItemTemplateStoreFast())
        {
            if (!proto || proto->Class != ITEM_CLASS_GLYPH)
                continue;

            for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            {
                SpellInfo const* useSpell = sSpellMgr->GetSpellInfo(proto->Spells[i].SpellId);
                if (!useSpell)
                    continue;

                for (uint8 eff = 0; eff < MAX_SPELL_EFFECTS; ++eff)
                {
                    if (useSpell->Effects[eff].Effect != SPELL_EFFECT_APPLY_GLYPH)
                        continue;

                    uint32 const glyphId = uint32(useSpell->Effects[eff].MiscValue);
                    GlyphPropertiesEntry const* gp = sGlyphPropertiesStore.LookupEntry(glyphId);
                    if (!gp || !sSpellMgr->GetSpellInfo(gp->SpellId))
                        continue;

                    GlyphDef& def = glyphDefs[glyphId];
                    if (def.ItemEntry)   // one item is enough; keep the first
                        continue;
                    def.SpellId        = gp->SpellId;
                    def.TypeFlags      = gp->TypeFlags;
                    def.ItemEntry      = proto->ItemId;
                    def.AllowableClass = uint32(proto->AllowableClass);
                }
            }
        }

        LOG_INFO("module", "mod-extraglyphs: {} glyphs mapped from glyph items", glyphDefs.size());
    }

    bool ClassMayUse(Player const* player, GlyphDef const& def)
    {
        return (def.AllowableClass & player->getClassMask()) != 0;
    }

    // Per-player state: glyph ids per spec, loaded once at login.
    struct ExtraGlyphState : public DataMap::Base
    {
        std::set<uint32> Glyphs[MAX_TALENT_SPECS];
    };

    ExtraGlyphState* State(Player* player)
    {
        return player->CustomData.GetDefault<ExtraGlyphState>("mod-extraglyphs");
    }

    bool IsSocketed(Player const* player, uint32 glyphId)
    {
        for (uint8 slot = 0; slot < MAX_GLYPH_SLOT_INDEX; ++slot)
            if (player->GetGlyph(slot) == glyphId)
                return true;
        return false;
    }

    // True when some socketed glyph of the active spec grants this spell, in
    // which case the aura is the socket's and must not be removed here.
    bool SpellHeldBySocket(Player const* player, uint32 spellId)
    {
        for (uint8 slot = 0; slot < MAX_GLYPH_SLOT_INDEX; ++slot)
            if (GlyphPropertiesEntry const* gp = sGlyphPropertiesStore.LookupEntry(player->GetGlyph(slot)))
                if (gp->SpellId == spellId)
                    return true;
        return false;
    }

    void ApplyGlyph(Player* player, uint32 glyphId)
    {
        auto it = glyphDefs.find(glyphId);
        if (it == glyphDefs.end())
            return;
        if (player->HasAura(it->second.SpellId))
            return;
        // Same trigger flags as Player::_LoadGlyphAuras and ActivateSpec.
        player->CastSpell(player, it->second.SpellId,
            TriggerCastFlags(TRIGGERED_FULL_MASK & ~(TRIGGERED_IGNORE_SHAPESHIFT | TRIGGERED_IGNORE_CASTER_AURASTATE)));
    }

    void UnapplyGlyph(Player* player, uint32 glyphId)
    {
        auto it = glyphDefs.find(glyphId);
        if (it == glyphDefs.end())
            return;
        if (SpellHeldBySocket(player, it->second.SpellId))
            return;
        player->RemoveAurasDueToSpell(it->second.SpellId);
    }

    void ApplyAllForActiveSpec(Player* player)
    {
        for (uint32 glyphId : State(player)->Glyphs[player->GetActiveSpec()])
            ApplyGlyph(player, glyphId);
    }

    void LoadState(Player* player)
    {
        ExtraGlyphState* state = State(player);
        for (auto& set : state->Glyphs)
            set.clear();

        QueryResult result = CharacterDatabase.Query(
            "SELECT spec, glyph FROM character_extra_glyphs WHERE guid = {}", player->GetGUID().GetCounter());
        if (!result)
            return;

        do
        {
            Field* fields = result->Fetch();
            uint8  const spec  = fields[0].Get<uint8>();
            uint32 const glyph = fields[1].Get<uint32>();
            if (spec < MAX_TALENT_SPECS && glyphDefs.count(glyph))
                state->Glyphs[spec].insert(glyph);
        } while (result->NextRow());
    }

    char const* TypeName(uint32 typeFlags)
    {
        return typeFlags & 1 ? "minor" : "major";
    }
}

class ExtraGlyphs_WorldScript : public WorldScript
{
public:
    ExtraGlyphs_WorldScript() : WorldScript("ExtraGlyphs_WorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }

    void OnStartup() override
    {
        // Item templates and DBCs exist by now (not at config-load time).
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS character_extra_glyphs ("
            "  guid INT UNSIGNED NOT NULL,"
            "  spec TINYINT UNSIGNED NOT NULL,"
            "  glyph INT UNSIGNED NOT NULL,"
            "  PRIMARY KEY (guid, spec, glyph)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci "
            "COMMENT='mod-extraglyphs: glyph effects beyond the six sockets'");
        LoadGlyphDefs();
    }
};

class ExtraGlyphs_PlayerScript : public PlayerScript
{
public:
    ExtraGlyphs_PlayerScript() : PlayerScript("ExtraGlyphs_PlayerScript",
        { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_AFTER_SPEC_SLOT_CHANGED, PLAYERHOOK_ON_DELETE_FROM_DB }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!cfg.Enable)
            return;
        LoadState(player);
        ApplyAllForActiveSpec(player);
    }

    // ActivateSpec has already swapped the socketed glyph auras and every
    // talent-driven passive; extras for the old spec come off here and the
    // new spec's go on, leaving alone any spell a socket of the new spec
    // still grants.
    void OnPlayerAfterSpecSlotChanged(Player* player, uint8 newSlot) override
    {
        if (!cfg.Enable)
            return;

        ExtraGlyphState* state = State(player);
        for (uint8 spec = 0; spec < MAX_TALENT_SPECS; ++spec)
        {
            if (spec == newSlot)
                continue;
            for (uint32 glyphId : state->Glyphs[spec])
                if (!state->Glyphs[newSlot].count(glyphId))
                    UnapplyGlyph(player, glyphId);
        }
        ApplyAllForActiveSpec(player);
    }

    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        trans->Append("DELETE FROM character_extra_glyphs WHERE guid = {}", guid);
    }
};

class ExtraGlyphs_CommandScript : public CommandScript
{
public:
    ExtraGlyphs_CommandScript() : CommandScript("ExtraGlyphs_CommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable extraGlyphsCommandTable =
        {
            { "list",    HandleListCommand,    SEC_PLAYER, Console::No },
            { "catalog", HandleCatalogCommand, SEC_PLAYER, Console::No },
            { "add",     HandleAddCommand,     SEC_PLAYER, Console::No },
            { "remove",  HandleRemoveCommand,  SEC_PLAYER, Console::No }
        };

        static ChatCommandTable commandTable =
        {
            { "extraglyphs", extraGlyphsCommandTable }
        };

        return commandTable;
    }

    static Player* Caller(ChatHandler* handler)
    {
        if (!cfg.Enable || !handler->GetSession())
            return nullptr;
        return handler->GetSession()->GetPlayer();
    }

    // Replies are one line per glyph, machine-readable for the addon:
    //   EG <kind> <glyphId> <spellId> <itemEntry> <major|minor> <state>
    // The addon takes the name and icon from GetSpellInfo(spellId) on its
    // own side, so nothing localised has to travel.
    static void SendGlyphLine(ChatHandler* handler, char const* kind, Player* player, uint32 glyphId, GlyphDef const& def)
    {
        char const* state = State(player)->Glyphs[player->GetActiveSpec()].count(glyphId) ? "extra"
                          : IsSocketed(player, glyphId)                                     ? "socketed"
                          :                                                                    "none";
        handler->PSendSysMessage("EG {} {} {} {} {} {}", kind, glyphId, def.SpellId, def.ItemEntry, TypeName(def.TypeFlags), state);
    }

    static bool HandleListCommand(ChatHandler* handler)
    {
        Player* player = Caller(handler);
        if (!player)
            return false;

        ExtraGlyphState* state = State(player);
        handler->PSendSysMessage("EG head {} {} {}", uint32(player->GetActiveSpec()), cfg.Max,
            uint32(state->Glyphs[player->GetActiveSpec()].size()));
        for (uint32 glyphId : state->Glyphs[player->GetActiveSpec()])
            if (auto it = glyphDefs.find(glyphId); it != glyphDefs.end())
                SendGlyphLine(handler, "glyph", player, glyphId, it->second);
        return true;
    }

    static bool HandleCatalogCommand(ChatHandler* handler)
    {
        Player* player = Caller(handler);
        if (!player)
            return false;

        ExtraGlyphState* state = State(player);
        handler->PSendSysMessage("EG head {} {} {}", uint32(player->GetActiveSpec()), cfg.Max,
            uint32(state->Glyphs[player->GetActiveSpec()].size()));
        for (auto const& [glyphId, def] : glyphDefs)
            if (ClassMayUse(player, def))
                SendGlyphLine(handler, "glyph", player, glyphId, def);
        return true;
    }

    static bool HandleAddCommand(ChatHandler* handler, uint32 glyphId)
    {
        Player* player = Caller(handler);
        if (!player)
            return false;

        auto it = glyphDefs.find(glyphId);
        if (it == glyphDefs.end() || !ClassMayUse(player, it->second))
        {
            handler->PSendSysMessage("EG err not a glyph your class can use.");
            return false;
        }
        GlyphDef const& def = it->second;

        if (player->GetLevel() < 15)
        {
            handler->PSendSysMessage("EG err glyphs unlock at level 15.");
            return false;
        }

        std::set<uint32>& extras = State(player)->Glyphs[player->GetActiveSpec()];
        if (extras.count(glyphId) || IsSocketed(player, glyphId))
        {
            handler->PSendSysMessage("EG err that glyph is already active.");
            return false;
        }
        if (cfg.Max && extras.size() >= cfg.Max)
        {
            handler->PSendSysMessage("EG err no room: {} extra glyphs is the limit.", cfg.Max);
            return false;
        }

        if (cfg.RequireItem)
        {
            if (!player->HasItemCount(def.ItemEntry, 1))
            {
                handler->PSendSysMessage("EG err you need the glyph in your bags.");
                return false;
            }
            player->DestroyItemCount(def.ItemEntry, 1, true);
        }

        extras.insert(glyphId);
        CharacterDatabase.Execute("INSERT IGNORE INTO character_extra_glyphs (guid, spec, glyph) VALUES ({}, {}, {})",
            player->GetGUID().GetCounter(), uint32(player->GetActiveSpec()), glyphId);
        ApplyGlyph(player, glyphId);

        LOG_DEBUG("module", "mod-extraglyphs: {} added glyph {} (spell {}) to spec {}",
            player->GetName(), glyphId, def.SpellId, player->GetActiveSpec());
        SendGlyphLine(handler, "added", player, glyphId, def);
        return true;
    }

    static bool HandleRemoveCommand(ChatHandler* handler, uint32 glyphId)
    {
        Player* player = Caller(handler);
        if (!player)
            return false;

        std::set<uint32>& extras = State(player)->Glyphs[player->GetActiveSpec()];
        auto it = glyphDefs.find(glyphId);
        if (it == glyphDefs.end() || !extras.count(glyphId))
        {
            handler->PSendSysMessage("EG err that is not one of your extra glyphs.");
            return false;
        }

        extras.erase(glyphId);
        CharacterDatabase.Execute("DELETE FROM character_extra_glyphs WHERE guid = {} AND spec = {} AND glyph = {}",
            player->GetGUID().GetCounter(), uint32(player->GetActiveSpec()), glyphId);
        UnapplyGlyph(player, glyphId);

        LOG_DEBUG("module", "mod-extraglyphs: {} removed glyph {} from spec {}",
            player->GetName(), glyphId, player->GetActiveSpec());
        SendGlyphLine(handler, "removed", player, glyphId, it->second);
        return true;
    }
};

void AddExtraGlyphsScripts()
{
    new ExtraGlyphs_WorldScript();
    new ExtraGlyphs_PlayerScript();
    new ExtraGlyphs_CommandScript();
}
