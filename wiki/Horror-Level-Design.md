# Horror Level Design

**Tools for building scary spaces — and making sure they're still playable by everyone.**

MonolithMesh includes 24 actions specifically for horror spatial analysis and encounter design, plus 6 accessibility actions and a comprehensive reporting system. This page covers how to use them together to design horror levels that are genuinely frightening, well-paced, and accessible to players with motor, vision, or cognitive impairments.

> **Context:** The horror tools and accessibility tools live side by side because they need to work together. A good horror game should be playable by everyone, including players with accessibility needs.

---

## Horror Spatial Analysis

These 8 actions give you a quantitative read on how scary a space is before any enemies or scripted events are involved. Pure spatial analysis.

### Sightline Analysis

```json
mesh_query({
  "action": "analyze_sightlines",
  "params": {
    "location": [1200, 800, 100],
    "fov": 90,
    "ray_count": 64
  }
})
```

Returns a claustrophobia score and blocked percentage at 5m, 10m, and 20m ranges. High blocked % at close range = claustrophobic. Low blocked % everywhere = exposed and vulnerable. Both are useful for horror — they just create different kinds of tension.

### Hiding Spots

```json
mesh_query({
  "action": "find_hiding_spots",
  "params": {
    "region_min": [0, 0, 0],
    "region_max": [2000, 2000, 300],
    "viewpoints": [[1000, 1000, 150], [500, 200, 150]]
  }
})
```

Grid-samples the region and scores each cell for concealment from the given viewpoints. Useful for both finding spots where players might hide AND where enemies could lurk.

### Ambush Points

```json
mesh_query({
  "action": "find_ambush_points",
  "params": {
    "path_points": [[0, 0, 0], [500, 0, 0], [500, 500, 0], [1000, 500, 0]],
    "lateral_range": 500
  }
})
```

Finds concealed positions within `lateral_range` of the path that have surprise angles (the player wouldn't see them coming until the last moment). Perfect for placing enemy spawn triggers or scripted scares.

### Escape Routes

```json
mesh_query({
  "action": "analyze_escape_routes",
  "params": {
    "location": [1000, 500, 100],
    "exit_tags": ["Exit", "SafeRoom"]
  }
})
```

Finds paths to tagged exits and scores them by threat exposure — how much of the path is in open sightlines, how many choke points it passes through, how long it takes. Players instinctively scan for exits. If there are none, panic goes up. If there's exactly one and it's through a narrow corridor, that's a different kind of scary.

### Zone Tension Classification

```json
mesh_query({
  "action": "classify_zone_tension",
  "params": {
    "location": [800, 400, 100],
    "radius": 1500
  }
})
```

Composite score across multiple factors (sightlines, lighting, hiding spots, exits, audio, prop disturbance). Returns one of: **calm**, **uneasy**, **tense**, **dread**, **panic**. The weights come from the active tension profile — see [[Genre Presets]] for how to customize these.

### Pacing Curves

```json
mesh_query({
  "action": "analyze_pacing_curve",
  "params": {
    "path_points": [[0, 0, 0], [300, 0, 0], [600, 200, 0], [900, 500, 0], [1200, 500, 0]]
  }
})
```

Samples tension along a path and returns a curve with detected scare points (sudden tension spikes) and rest points (tension valleys). Use this to verify that your level has proper tension-release rhythm — constant high tension is exhausting, not scary.

### Dead Ends

```json
mesh_query({
  "action": "find_dead_ends",
  "params": {
    "region_min": [-1000, -1000, 0],
    "region_max": [3000, 3000, 300]
  }
})
```

Navmesh flood-fill that finds single-exit regions. Dead ends are powerful horror tools — the player realizes they have to go back the way they came — but they can also be frustrating traps if overused or if the player has mobility impairments.

---

## Encounter Design

The encounter design tools build on the spatial analysis to help you place enemies, plan patrol routes, and evaluate the overall pacing of a level.

### The Capstone: `design_encounter`

```json
mesh_query({
  "action": "design_encounter",
  "params": {
    "region": {
      "min": [0, 0, 0],
      "max": [3000, 2000, 300]
    },
    "archetype": "stalker"
  }
})
```

This is the big composite action. It evaluates spawn points in the region, suggests patrol routes, analyzes exits and sightlines, and returns a complete encounter design. The `archetype` parameter shapes the AI behavior pattern:

- **`stalker`** — Follows player paths at a distance, appears in peripheral vision, retreats when confronted
- **`patrol`** — Predictable routes the player can learn and avoid
- **`ambusher`** — Hides near path chokepoints, waits for proximity trigger

### Patrol Routes

```json
mesh_query({
  "action": "suggest_patrol_route",
  "params": {
    "path_points": [[0, 0, 0], [1000, 0, 0], [1000, 1000, 0]],
    "archetype": "stalker"
  }
})
```

Generates navmesh-valid patrol waypoints that match the archetype's behavior pattern. A stalker's route shadows the player path. A patrol's route is a loop. An ambusher's route connects hiding spots near the path.

### Safe Room Evaluation

```json
mesh_query({
  "action": "evaluate_safe_room",
  "params": {
    "volume_name": "BV_SafeRoom_01"
  }
})
```

Scores a room for defensibility (sightlines to entrances, number of exits, cover positions) AND accessibility amenity scoring (space for wheelchair turning, rest point availability, cognitive simplicity). A good safe room for a player with accessibility needs might need wider doorways and fewer confusing paths than one designed for a typical FPS player.

### Scare Sequences

```json
mesh_query({
  "action": "generate_scare_sequence",
  "params": {
    "path_points": [[0, 0, 0], [500, 0, 0], [1000, 300, 0], [1500, 600, 0], [2000, 600, 0]],
    "style": "slow_burn"
  }
})
```

4 styles, each with different tension curves:
- **`slow_burn`** — Gradual buildup, subtle environmental cues, delayed payoff
- **`escalating`** — Each scare bigger than the last, crescendo structure
- **`relentless`** — High intensity throughout, no safe moments, exhausting (use sparingly)
- **`single_peak`** — Long calm setup, one massive scare, then relief

### Horror Intensity Validation

```json
mesh_query({
  "action": "validate_horror_intensity",
  "params": {
    "path_points": [[0, 0, 0], [500, 0, 0], [1000, 0, 0], [1500, 0, 0]]
  }
})
```

Audits the path against accessibility intensity caps. Flags sections that are too intense for too long without rest, or transitions that are too abrupt. This isn't about making the game less scary — it's about making sure the pacing allows players with lower stress tolerance to still enjoy the experience. A well-paced horror game is scarier than a constantly overwhelming one anyway.

---

## Accessibility Integration

The 6 accessibility actions are designed to be run alongside or after the horror design pass.

### The Full Report

```json
mesh_query({
  "action": "generate_accessibility_report",
  "params": {
    "start": [0, 0, 0],
    "end": [3000, 2000, 0],
    "profile": "motor"
  }
})
```

Profiles: `motor`, `vision`, `cognitive`, or omit for all three. Returns:
- **Path width analysis** — Wheelchair clearance along the entire route
- **Navigation complexity** — Turn count, decision points, backtracking required
- **Visual contrast** — Interactable visibility against backgrounds
- **Rest point spacing** — Distance between safe zones
- **Interactive reach** — Can controls be reached from a seated position

Each section gets an A-F grade. The overall grade is the lowest individual grade — one failing section fails the whole path.

### Individual Checks

Sometimes you just need to verify one thing:

```json
// Is this corridor wide enough for a wheelchair?
mesh_query({
  "action": "validate_path_width",
  "params": { "start": [0, 0, 0], "end": [1000, 0, 0], "min_width": 120 }
})

// Are rest points close enough together?
mesh_query({
  "action": "find_rest_points",
  "params": { "start": [0, 0, 0], "end": [3000, 0, 0], "max_gap": 500 }
})

// Can a colorblind player see the interactive elements?
mesh_query({
  "action": "analyze_visual_contrast",
  "params": { "location": [500, 300, 100], "forward": [1, 0, 0] }
})
```

### Accessibility Report (generate_hospice_report)

```json
mesh_query({
  "action": "generate_hospice_report",
  "params": {
    "start": [0, 0, 0],
    "end": [3000, 2000, 0],
    "profile": "motor"
  }
})
```

5-section accessibility audit with more conservative thresholds than the general accessibility report — wider paths, shorter distances between rest points, lower cognitive complexity caps. Designed for players with diverse accessibility needs. Returns an A-F grade with specific recommendations.

---

## Putting It All Together

Here's a typical workflow for designing a horror level section:

### 1. Analyze the space
```
analyze_sightlines -> classify_zone_tension -> find_dead_ends
```
Understand what you're working with. Where's it naturally tense? Where's it too open?

### 2. Plan encounters
```
design_encounter -> suggest_patrol_route -> evaluate_spawn_point
```
Place the scary stuff. Use spatial analysis to pick spots with maximum impact.

### 3. Check pacing
```
analyze_pacing_curve -> evaluate_encounter_pacing -> generate_scare_sequence
```
Make sure the tension-release rhythm works. No sustained maxima without rest.

### 4. Validate intensity
```
validate_horror_intensity
```
Run the accessibility intensity cap audit. Adjust sections that are too much for too long.

### 5. Accessibility pass
```
generate_accessibility_report -> generate_hospice_report
```
Make sure the space is physically navigable. Fix width issues, add rest points, check contrast.

### 6. Audio design
```
get_surface_materials -> analyze_room_acoustics -> get_stealth_map -> can_ai_hear_from
```
Set up the audio environment. Make sure the stealth gameplay works with the surface materials in the room.

### 7. Lighting
```
get_light_coverage -> find_dark_corners -> analyze_light_transitions -> suggest_light_placement
```
Light it for mood AND playability. Flag harsh transitions that might disorient players.

---

## Design Philosophy

A few principles that shaped these tools:

**Scary and accessible aren't opposites.** A well-paced horror experience with proper rest points and clear navigation is scarier than a confusing mess. When players feel lost and frustrated, they're not scared — they're annoyed. When they know where to go but dread going there, that's horror.

**Quantify, don't dictate.** These tools score spaces — they don't force you to change them. A dead-end that fails the accessibility check might be exactly the right design choice for a particular moment. The tools give you data; you make the creative calls.

**Players with accessibility needs are players.** They want the same game everyone else gets, possibly with adjusted intensity pacing and wider corridors. The accessibility tools exist to make that possible without dumbing anything down.

---

## Related Pages

- [[Mesh Module]] — Full MonolithMesh overview
- [[Genre Presets]] — Customizing tension profiles and storytelling patterns
- [[Procedural Geometry]] — Generating structures and props procedurally
- [[Tool Reference]] — Action signatures and parameters
