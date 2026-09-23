# mod-extraglyphs

Glyph effects beyond the six sockets.

Socketing a glyph does two things: it writes the glyph id into one of six
update fields the client draws its glyph UI from, and it casts
`GlyphProperties.SpellId` on the player as a passive aura. Only the second is
the effect. The six is a client limit - `PLAYER_FIELD_GLYPHS_1` is a six-wide
field in a protocol layout shared with `Wow.exe` - so instead of fighting it,
this module keeps its own list of extra glyphs per character and per talent
spec and applies their spells the same way the core applies socketed ones:
`CastSpell` on login and on spec switch, `RemoveAurasDueToSpell` on removal.

Nothing goes through the socket path. The glyph item's own use spell
(`SPELL_EFFECT_APPLY_GLYPH`) still targets a socket and is left alone; extras
are added by command from the companion addon panel, over the addon command
channel:

    extraglyphs list                active extras for the current spec
    extraglyphs catalog             every glyph the class can use
    extraglyphs add <glyphId>       apply one, consuming the glyph item
    extraglyphs remove <glyphId>    drop one; the glyph is lost, as with sockets

Which glyphs a class may use is not in `GlyphProperties.dbc`. It comes from
the glyph items: each one's use spell names the glyph id in its `APPLY_GLYPH`
effect, and the item's `AllowableClass` says who may use it. That map is
built at startup and is also how `add` knows which item to consume.

Storage is one row per (guid, spec, glyph) in `character_extra_glyphs`,
created on first start, dropped with the character.

## Configuration (`mod_extraglyphs.conf`)

| key | meaning |
| --- | --- |
| `ExtraGlyphs.Enable` | master switch |
| `ExtraGlyphs.Max` | extra glyphs per spec; 0 for no limit |
| `ExtraGlyphs.RequireItem` | consume the glyph item, as socketing would |

Commands are `SEC_PLAYER`, `Console::No`, and only ever act on the caller.

## Requirements

The `ExtraGlyphs` client addon for the panel; the commands also work typed
into the chat box.

## Licence

GNU Affero General Public License v3.0, the licence AzerothCore and its
modules use. See [LICENSE](LICENSE).
