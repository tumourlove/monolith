# Genre Presets

**Horror is just the default. The preset system is genre-agnostic.**

Monolith ships with horror presets because that's the genre of the project it was built for. But the underlying systems — storytelling patterns, acoustic profiles, tension profiles — are abstract enough to work with any genre. You can author presets for fantasy dungeons, sci-fi corridors, cozy cottages, or post-apocalyptic wastelands. The horror-specific stuff (scare placement, hiding spots, monster spawns) still uses the same analysis engines; they just score differently with different tension weights.

---

## What Ships By Default

Out of the box, Monolith includes:

### Storytelling Patterns (5)
Horror environmental storytelling presets used by `place_storytelling_scene`:
- **violence** — Overturned furniture, blood decals, broken glass, scattered items
- **abandoned** — Dust, cobwebs, fallen objects, water damage indicators
- **dragged** — Blood trail, scratch marks, drag path with displaced objects
- **medical** — Gurney, IV stand, scattered instruments, biohazard
- **corruption** — Organic growth patterns, discoloration, structural decay

### Acoustic Profiles
12 surface types for horror audio:
- Concrete, tile, metal, wood, carpet, glass, gravel, water, organic, dirt, fabric, plastic
- Each has loudness factor, reverb contribution, and AI detection modifier

### Tension Profiles
Default horror tension weights used by `classify_zone_tension`:
- Sightline openness, lighting level, hiding spot density, exit count, audio ambience, prop disturbance level

---

## Preset JSON Formats

### Storytelling Pattern

```json
{
  "name": "ritual_site",
  "genre": "fantasy",
  "elements": [
    {"type": "decal", "material": "/Game/Materials/M_RuneCircle", "count": 1, "placement": "floor_center"},
    {"type": "prop", "mesh": "/Game/Meshes/SM_Candle", "count": 8, "placement": "circle", "radius": 200},
    {"type": "prop", "mesh": "/Game/Meshes/SM_Skull", "count": 3, "placement": "scatter", "radius": 150},
    {"type": "decal", "material": "/Game/Materials/M_Scorch", "count": 4, "placement": "scatter", "radius": 300}
  ],
  "intensity_scale": {
    "low": {"count_multiplier": 0.5, "skip_types": ["decal"]},
    "medium": {},
    "high": {"count_multiplier": 1.5, "extra_elements": [
      {"type": "prop", "mesh": "/Game/Meshes/SM_BrokenAltar", "count": 1, "placement": "floor_center"}
    ]}
  }
}
```

### Acoustic Profile

```json
{
  "name": "dungeon_stone",
  "genre": "fantasy",
  "surfaces": {
    "stone_floor": {"loudness": 0.7, "reverb_contribution": 0.8, "ai_detection_modifier": 1.2},
    "stone_wall": {"loudness": 0.6, "reverb_contribution": 0.9, "ai_detection_modifier": 1.0},
    "wooden_door": {"loudness": 0.8, "reverb_contribution": 0.2, "ai_detection_modifier": 1.5},
    "water_puddle": {"loudness": 0.9, "reverb_contribution": 0.1, "ai_detection_modifier": 1.8},
    "moss": {"loudness": 0.2, "reverb_contribution": 0.05, "ai_detection_modifier": 0.3},
    "iron_grate": {"loudness": 1.0, "reverb_contribution": 0.3, "ai_detection_modifier": 2.0}
  },
  "default_rt60": 1.8,
  "reverb_preset_suggestion": "LargeStoneRoom"
}
```

### Tension Profile

```json
{
  "name": "fantasy_dungeon",
  "genre": "fantasy",
  "weights": {
    "sightline_openness": 0.15,
    "lighting_level": 0.25,
    "hiding_spot_density": 0.1,
    "exit_count": 0.2,
    "audio_ambience": 0.1,
    "prop_disturbance": 0.05,
    "elevation_change": 0.15
  },
  "thresholds": {
    "calm": 0.0,
    "uneasy": 0.25,
    "tense": 0.5,
    "dread": 0.75,
    "panic": 0.9
  }
}
```

---

## Step-by-Step: Creating a Fantasy Dungeon Preset Pack

Let's walk through building a complete preset pack for a fantasy dungeon crawler.

### 1. Create storytelling patterns

```json
mesh_query({
  "action": "create_storytelling_pattern",
  "params": {
    "name": "ritual_site",
    "elements": [
      {"type": "decal", "material": "/Game/Fantasy/M_RuneCircle", "count": 1, "placement": "floor_center"},
      {"type": "prop", "mesh": "/Game/Fantasy/SM_Candle", "count": 8, "placement": "circle", "radius": 200},
      {"type": "prop", "mesh": "/Game/Fantasy/SM_Skull", "count": 3, "placement": "scatter", "radius": 150}
    ]
  }
})
```

Repeat for other patterns — `treasure_hoard`, `prison_cell`, `collapsed_tunnel`, `alchemy_lab`, etc.

### 2. Create acoustic profile

```json
mesh_query({
  "action": "create_acoustic_profile",
  "params": {
    "name": "dungeon_stone",
    "surfaces": {
      "stone_floor": {"loudness": 0.7, "reverb_contribution": 0.8, "ai_detection_modifier": 1.2},
      "stone_wall": {"loudness": 0.6, "reverb_contribution": 0.9, "ai_detection_modifier": 1.0},
      "wooden_door": {"loudness": 0.8, "reverb_contribution": 0.2, "ai_detection_modifier": 1.5},
      "water_puddle": {"loudness": 0.9, "reverb_contribution": 0.1, "ai_detection_modifier": 1.8}
    }
  }
})
```

### 3. Create tension profile

```json
mesh_query({
  "action": "create_tension_profile",
  "params": {
    "name": "fantasy_dungeon",
    "weights": {
      "sightline_openness": 0.15,
      "lighting_level": 0.25,
      "hiding_spot_density": 0.1,
      "exit_count": 0.2,
      "audio_ambience": 0.1,
      "prop_disturbance": 0.05,
      "elevation_change": 0.15
    }
  }
})
```

### 4. Export the preset pack

```json
mesh_query({
  "action": "export_genre_preset",
  "params": {
    "path": "D:/Presets/fantasy_dungeon_pack.json"
  }
})
```

This bundles all your custom storytelling patterns, acoustic profiles, and tension profiles into a single JSON file.

### 5. Test it

Use the horror analysis tools with your new presets active:

```json
mesh_query({
  "action": "classify_zone_tension",
  "params": {
    "location": [0, 0, 100],
    "radius": 2000
  }
})
```

The tension classification will use your custom weights instead of the horror defaults.

```json
mesh_query({
  "action": "place_storytelling_scene",
  "params": {
    "location": [500, 300, 0],
    "pattern": "ritual_site",
    "intensity": "high"
  }
})
```

### 6. Share with others

The exported JSON file is self-contained. Other Monolith users import it with:

```json
mesh_query({
  "action": "import_genre_preset",
  "params": {
    "path": "D:/Presets/fantasy_dungeon_pack.json",
    "merge_mode": "overwrite"
  }
})
```

`merge_mode` options:
- `"overwrite"` — Replace existing presets with the same name
- `"merge"` — Keep existing, only add new ones
- `"skip"` — Skip any names that already exist

---

## Browse What's Available

```json
mesh_query({ "action": "list_genre_presets" })
mesh_query({ "action": "list_storytelling_patterns" })
mesh_query({ "action": "list_acoustic_profiles" })
```

---

## Ideas for Other Genres

The system is flexible enough for anything. Some ideas:

- **Sci-fi space station** — Metal surfaces (high loudness, high reverb), flickering light patterns, hull breach storytelling, zero-gravity prop scatter
- **Cozy sim** — Low tension weights across the board, warm acoustic profiles, tidy/slightly_messy disturbance levels only
- **Post-apocalyptic** — High prop disturbance, concrete/rubble acoustics, scavenger camp storytelling patterns
- **Stealth game** — Heavy acoustic profile emphasis, detailed surface loudness maps, guard patrol integration
- **Underwater** — Custom reverb with long RT60, muffled propagation, pressure-based tension

The analysis tools (sightlines, tension, pacing curves, escape routes) work regardless of genre. A "tense corridor" in a horror game and a "suspenseful approach" in a stealth game use the same spatial math — just with different weights on what makes a space feel dangerous.

---

## Related Pages

- [[Mesh Module]] — Full MonolithMesh overview
- [[Tool Reference]] — Action signatures and parameters
- [[Horror Level Design]] — How the horror-specific presets are used in practice
