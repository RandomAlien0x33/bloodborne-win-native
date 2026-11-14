#!/usr/bin/env python3
"""The map tour's points (bbport_tour.cpp) from the game's own files.

usage: make_points.py <game dir (CUSA03173)> <out file> [all | spacing in metres]

The tour warps to a map (the game's map warp lands at the map's start) and then puts the character
at each point of that map by its position. The points, per map:

- the lamps' arrival points first: every lamp is set up by the common event 7000, started from its
  map's event script (event/mAA_BB_00_00.emevd.dcx) as InitializeEvent(slot, 7000, arrival, lamp,
  condition, lit); `arrival` (lamp - 1000, a lamp messenger part) is where the character appears;
- `all` (the default): every character of the map (map/MapStudio/mAA_BB_00_00.msb.dcx): enemies
  and friendly NPCs, with an entity id or not, characters within 7 m of each other as one point;
  the messengers (c90xx: at lamps, in the Dream, by gravestones and notes) are left out;
- a spacing instead: regions with an entity id (event areas: boss arenas, fog walls, triggers) and
  characters, one per `spacing` metres;
- the bosses in the same order, kind `boss`: a character with a health bar (event instruction 2003:11: state,
  entity, slot, NpcName id). The tour looks round longer there.

Before it starts the tour clears every boss's "defeated" flag (the first instruction of the event
that shows the boss's health bar ends it when that flag is on, 1003:02), so killed bosses stand in
their arenas again; saves are frozen meanwhile.

Maps go roughly in the order a player meets them (STORY_ORDER; others after them); in a map the
tour goes nearest-first from its first lamp. Chalice dungeon maps (m29) are left out.

EMEVD (Bloodborne, 64-bit): event count and offset at 0x10, instruction count and offset at 0x20,
arguments' offset at 0x78; an event (0x30 bytes): id, instruction count, first instruction's offset
from the instructions (i64 each); an instruction (0x20 bytes): bank (i32), id (i32), its arguments'
length and offset (i64 each), a layer offset (i64); InitializeEvent is 2000:00.

MSB (64-bit): param lists from 0x10, each: version (i32), count + 1 (i32), name offset (i64),
`count` entry offsets and the next list's offset (i64). POINT_PARAM_ST entry: shape at 0x10,
position at 0x14, entity data offset at 0x48 (entity id first). PARTS_PARAM_ST entry: name offset
at 8 (the name starts with the model, c5000_0000), type at 0x14 (2: a character), position at 0x28,
entity data offset at 0xb0.

Texts (msg/engus/item.msgbnd.dcx): BND4, entries of 0x24 bytes from 0x40 (size at +8, data offset
and id at +0x18); FMG: group count at 0xc, string offsets' offset at 0x18, groups from 0x28 (first
index, first id, last id, pad; i32 each), string offsets i64, UTF-16 strings. NpcName is FMG 18,
PlaceName FMG 19 (area * 100 + block * 10). Characters other than bosses have no name in the game.

Output lines: `unflag N` (cleared before the tour), `area block entity kind x y z<TAB>title` (the
position in the map's file, the frame of the game's positions; entity -1: none; the title the
tour shows there, in English).
"""
import os
import struct
import sys
import zlib

# Map (area, block) in the order the game leads a player through them: Central Yharnam, Hunter's
# Dream, Cathedral Ward, Old Yharnam, Hemwick Charnel Lane, Forbidden Woods, Byrgenwerth,
# Yahar'gul, Upper Cathedral Ward, Cainhurst, Abandoned Old Workshop, Nightmare Frontier,
# Nightmare of Mensis; The Old Hunters: Hunter's Nightmare, Research Hall, Fishing Hamlet.
STORY_ORDER = [(24, 1), (21, 0), (24, 0), (23, 0), (22, 0), (27, 0), (32, 0), (28, 0), (24, 2),
               (25, 0), (21, 1), (33, 0), (26, 0), (34, 0), (35, 0), (36, 0)]
MESSENGERS = 'c90'  # c9000-c9099: the small messengers
TEST_LAYOUTS = ('m29_09_00_00', 'm29_01_99_00')
SAME_PLACE = 7.0  # metres: characters this close are one point


def load(path):
    data = open(path, 'rb').read()
    return zlib.decompress(data[data.find(b'DCA\0') + 8:])


def events(data):
    """(event id, [(bank, id, [i32 arguments])]) of an event script."""
    event_count, event_at = struct.unpack_from('<qq', data, 0x10)
    instruction_at = struct.unpack_from('<q', data, 0x28)[0]
    arguments = struct.unpack_from('<q', data, 0x78)[0]
    for i in range(event_count):
        eid, count, first = struct.unpack_from('<qqq', data, event_at + i * 0x30)
        body = []
        for k in range(count):
            bank, iid, length, offset = struct.unpack_from('<iiqq', data, instruction_at + first + k * 0x20)
            args = data[arguments + offset:arguments + offset + length]
            body.append((bank, iid, [struct.unpack_from('<i', args, j)[0] for j in range(0, len(args) - 3, 4)]))
        yield eid, body


def utf16(data, at):
    end = at
    while data[end:end + 2] != b'\0\0':
        end += 2
    return data[at:end].decode('utf-16le', 'replace')


def param_lists(data):
    lists, at = {}, 0x10
    while at:
        _, count, name = struct.unpack_from('<iiq', data, at)
        offsets = struct.unpack_from('<%dq' % count, data, at + 16)
        lists[utf16(data, name)] = offsets[:-1]
        at = offsets[-1]
    return lists


def entities(data):
    """(entity, kind, position, model) of the map's regions with an entity id and its characters."""
    lists = param_lists(data)
    entity = lambda e, at: struct.unpack_from('<i', data, e + struct.unpack_from('<q', data, e + at)[0])[0]
    for e in lists['POINT_PARAM_ST']:
        if entity(e, 0x48) > 0:
            yield entity(e, 0x48), 'region', struct.unpack_from('<3f', data, e + 0x14), ''
    for e in lists['PARTS_PARAM_ST']:
        if struct.unpack_from('<i', data, e + 0x14)[0] == 2:
            model = utf16(data, e + struct.unpack_from('<q', data, e + 8)[0])[:5]
            yield max(entity(e, 0xb0), -1), 'character', struct.unpack_from('<3f', data, e + 0x28), model


def enemies(data):
    """(entity, position, model, NPC parameters without the depth prefix) of the map's
    characters."""
    lists = param_lists(data)
    for e in lists['PARTS_PARAM_ST']:
        if struct.unpack_from('<i', data, e + 0x14)[0] != 2:
            continue
        model = utf16(data, e + struct.unpack_from('<q', data, e + 8)[0])[:5]
        entity = struct.unpack_from('<i', data, e + struct.unpack_from('<q', data, e + 0xb0)[0])[0]
        npc = struct.unpack_from('<i', data, e + struct.unpack_from('<q', data, e + 0xb8)[0] + 0xc)[0]
        yield max(entity, -1), struct.unpack_from('<3f', data, e + 0x28), model, npc % 1000000


def texts(path, fmg_id):
    """{id: text} of one FMG of a message archive."""
    data = load(path)
    files = {}
    for i in range(struct.unpack_from('<i', data, 0xc)[0]):
        size = struct.unpack_from('<q', data, 0x40 + i * 0x24 + 8)[0]
        offset, fid = struct.unpack_from('<ii', data, 0x40 + i * 0x24 + 0x18)
        files[fid] = data[offset:offset + size]
    fmg, out = files[fmg_id], {}
    strings = struct.unpack_from('<q', fmg, 0x18)[0]
    for g in range(struct.unpack_from('<i', fmg, 0xc)[0]):
        index, first, last, _ = struct.unpack_from('<iiii', fmg, 0x28 + g * 16)
        for k in range(last - first + 1):
            at = struct.unpack_from('<q', fmg, strings + (index + k) * 8)[0]
            if at:
                out[first + k] = utf16(fmg, at).strip()
    return out


def distance2(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b))


def dungeons(root, studio, npc_names):
    """The chalice dungeons, after the main game: one point per enemy that looks new there, a
    model the main game does not have, or a main game model with a second set of textures
    (chr/cXXXX_2.tpf, as the fire dog), at a placement with NPC parameters the main game does not
    use (its dungeon version; parameters differing only in strength, weapons or behaviour are one
    look, so one point). The parameters are compared without their depth prefix (10100080,
    110100080 ... are 100080). Each dungeon folder (map/MapStudio/m29_AA_BB_00/) holds its ready-made layouts
    (m29_AA_BB_NN); the fewest layouts that together have every such enemy are chosen, one point
    per enemy, at its first placement there."""
    main_enemies = set()
    for file in os.listdir(studio):
        if file.endswith('.msb.dcx') and not file.startswith('m29'):
            main_enemies.update((m, npc) for _, _, m, npc in enemies(load(os.path.join(studio, file))))
    main_models = {m for m, _ in main_enemies}
    models = {}

    def looks_new(m):
        return m not in main_models or os.path.exists(os.path.join(root, 'chr', m + '_2.tpf.dcx'))

    def has_model(m):
        if m not in models:
            models[m] = os.path.exists(os.path.join(root, 'chr', m + '.chrbnd.dcx'))
        return models[m]

    layouts = {}
    for folder in sorted(os.listdir(studio)):
        path = os.path.join(studio, folder)
        # m29_09, m29_01_99: characters in rows, test maps, not dungeons.
        if not (folder.startswith('m29') and os.path.isdir(path)) or folder in TEST_LAYOUTS:
            continue
        for file in sorted(os.listdir(path)):
            if not file.endswith('.msb.dcx'):
                continue
            first = {}
            found = list(enemies(load(os.path.join(path, file))))
            # A layout with a character whose model is missing from chr/ (test layouts) crashes
            # the game when it is loaded.
            if any(not has_model(m) for _, _, m, _ in found):
                continue
            # Test layouts: the characters stand in rows on whole metres at height 0.
            in_rows = sum(1 for _, p, _, _ in found
                          if p[1] == 0.0 and p[0] == int(p[0]) and p[2] == int(p[2]))
            if len(found) >= 3 and in_rows >= 0.8 * len(found):
                continue
            for e, p, m, npc in found:
                if npc and (m, npc) not in main_enemies and not m.startswith(MESSENGERS) and looks_new(m):
                    # A placement with an entity id first: events (a boss's) refer to it.
                    if m not in first or (first[m][0] <= 0 < e):
                        first[m] = (e, p, npc)
            if first:
                layouts[file[:-len('.msb.dcx')]] = first
    wanted = set().union(*(set(f) for f in layouts.values())) if layouts else set()
    chosen, covered = [], set()
    while covered != wanted:
        name = max(layouts, key=lambda n: (len(set(layouts[n]) - covered), n.endswith('_00'), n))
        chosen.append(name)
        covered |= set(layouts[name])
    boss_names = {}
    for name in chosen:
        script = os.path.join(root, 'event', name[:-2] + '00', name + '.emevd.dcx')
        if os.path.exists(script):
            for _, body in events(load(script)):
                for bank, iid, args in body:
                    if (bank, iid) == (2003, 11) and len(args) >= 4 and args[3] in npc_names:
                        boss_names.setdefault((name, args[1]), npc_names[args[3]])
    lines, total, done = [], 0, set()
    for name in chosen:
        new = [(m, e, p, npc) for m, (e, p, npc) in sorted(layouts[name].items()) if m not in done]
        done |= {k[0] for k in new}
        # Nearest first, bosses included (see the main maps).
        order, group = [], list(new)
        while group:
            near = min(group, key=lambda k: distance2(k[2], order[-1][2]) if order else 0)
            order.append(near)
            group.remove(near)
        lines += ['# %s chalice dungeon: %d points (%d bosses)' % (name, len(order), sum(1 for k in order if (name, k[1]) in boss_names))]
        for m, e, p, npc in order:
            boss = boss_names.get((name, e))
            kind = 'boss' if boss else 'character'
            title = ('%s (chalice %s)' % (boss, name) if boss
                     else 'Chalice %s: %s, %d' % (name, m, npc))
            lines.append('%s %d %s %.2f %.2f %.2f\t%s' % ((name, e, kind) + tuple(p) + (title,)))
        total += len(order)
    return lines, total


def main():
    game, out = sys.argv[1], sys.argv[2]
    mode = sys.argv[3] if len(sys.argv) > 3 else 'all'
    every = mode == 'all'
    spacing = SAME_PLACE if every else float(mode)
    root = os.path.join(game, 'dvdroot_ps4')
    item_texts = os.path.join(root, 'msg', 'engus', 'item.msgbnd.dcx')
    npc_names, place_names = texts(item_texts, 18), texts(item_texts, 19)
    arrivals, boss_names, defeated = set(), {}, []
    for file in sorted(os.listdir(os.path.join(root, 'event'))):
        if not (file.startswith('m') and file.endswith('_00.emevd.dcx')) or file.startswith('m29'):
            continue
        for _, body in events(load(os.path.join(root, 'event', file))):
            bars = [args for bank, iid, args in body
                    if (bank, iid) == (2003, 11) and len(args) >= 4 and args[3] in npc_names]
            for args in bars:
                boss_names.setdefault(args[1], npc_names[args[3]])
            ends = [args[1] for bank, iid, args in body if (bank, iid) == (1003, 2) and len(args) >= 2]
            # A map's own flag (1AAB....); a global one (9471) is the game's progress, not a boss's.
            if bars and ends and ends[0] >= 10000000 and ends[0] not in defeated:
                defeated.append(ends[0])
            for bank, iid, args in body:
                if (bank, iid) == (2000, 0) and len(args) >= 6 and args[1] == 7000:
                    arrivals.add(args[2])
    lines, total = ['unflag %d' % f for f in defeated], 0
    studio = os.path.join(root, 'map', 'MapStudio')
    files = [f for f in os.listdir(studio) if f.endswith('_00.msb.dcx') and not f.startswith('m29')]
    rank = lambda f: (STORY_ORDER.index((int(f[1:3]), int(f[4:6])))
                      if (int(f[1:3]), int(f[4:6])) in STORY_ORDER else len(STORY_ORDER), f)
    for file in sorted(files, key=rank):
        area, block = int(file[1:3]), int(file[4:6])
        found = list(entities(load(os.path.join(studio, file))))
        # The lamps by number: a map's first lamp is the one the player reaches first.
        lamps = sorted((e, 'lamp', p) for e, _, p, _ in found if e in arrivals)
        kept = list(lamps)
        for e, kind, p, model in found:
            if e in arrivals or model.startswith(MESSENGERS) or (every and kind == 'region'):
                continue
            if e in boss_names:
                kept.append((e, 'boss', p))
            elif all(distance2(p, q) > spacing ** 2 for _, _, q in kept):
                kept.append((e, kind, p))
        if not kept:
            continue
        # Nearest first from the first lamp (or the first point), bosses included: a boss stands in
        # its arena all along, so the points next to it show it anyway.
        order, group = [kept[0]], kept[1:]
        while group:
            near = min(group, key=lambda k: distance2(k[2], order[-1][2]))
            order.append(near)
            group.remove(near)
        place = place_names.get(area * 100 + block * 10, 'm%02d_%02d' % (area, block))
        characters = sum(1 for k in order if k[1] == 'character')
        seen = {'character': 0}

        def title(e, kind):
            if kind == 'lamp':
                return 'Lamp: %s' % place
            if kind == 'boss':
                return '%s (%s)' % (boss_names[e], place)
            if kind == 'character':
                seen['character'] += 1
                return '%s: point %d of %d' % (place, seen['character'], characters)
            return '%s: %s %d' % (place, kind, e)

        bosses = sum(1 for k in order if k[1] == 'boss')
        lines += ['# m%02d_%02d %s: %d points (%d lamps, %d bosses)'
                  % (area, block, place, len(order), len(lamps), bosses)]
        lines += ['%d %d %d %s %.2f %.2f %.2f\t%s' % ((area, block, e, kind) + tuple(p) + (title(e, kind),))
                  for e, kind, p in order]
        total += len(order)
    if every:
        dungeon_lines, dungeon_total = dungeons(root, studio, npc_names)
        lines += dungeon_lines
        total += dungeon_total
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        heading = 'every character' if every else 'one per %g m' % spacing
        f.write('# %d points, %s\n' % (total, heading) + '\n'.join(lines) + '\n')
    print('%d points, %d bosses\' defeated flags in %s' % (total, len(defeated), out))


if __name__ == '__main__':
    main()
