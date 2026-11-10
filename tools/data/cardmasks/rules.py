"""Per-card team rules (research prototype).

A rule: hue window [h0, h1] in degrees (wrapping allowed: h0 > h1), minimum saturation,
value range, optional region rectangles (x0, y0, x1, y1 inclusive; union) and
exclusion rectangles. Team = character (not backdrop) AND any rule matches.
'status' is this study's verdict for the card:
  clear   - the team parts have their own hue; backdrop separation by segmentation suffices
  region  - needs a rectangle (team hue shared with another part of the character or the backdrop)
  mask    - needs a hand pixel mask (team hue shared with the backdrop or other parts at the edges)
  none    - no team part on the card (or not one worth recolouring)
  owner   - the card and the sprite disagree; the owner should pick what is team
"""

def R(h0, h1, s=0.35, v0=0.0, v1=1.0, rect=None, ex=None):
    return dict(h0=h0, h1=h1, s=s, v0=v0, v1=v1, rect=rect, ex=ex)

BLUE = R(200, 255, 0.35)
RED = R(340, 18, 0.5)
GREEN = R(88, 170, 0.4)
ORANGE = R(3, 30, 0.55)

RULES = {
    # ---- King
    "K_King": dict(r=[R(205, 255, 0.35)], status="clear", note="cape and legs (blue); the tabard is white/red in the art, team in the sprite"),
    "K_King_F": dict(r=[R(205, 255, 0.35)], status="clear", note="dress (blue)"),
    "K_Engineer": dict(r=[R(205, 255, 0.35)], status="clear", note="tunic (blue)"),
    "K_Swordsman": dict(r=[R(205, 255, 0.35)], status="clear", note="tabard and the shield's blue quarters"),
    "K_Archer": dict(r=[R(205, 255, 0.3)], status="clear", note="navy sleeves/legs; the sprite's team torso is a brown vest in the art"),
    "K_Knight": dict(r=[R(205, 255, 0.4, 0.0, 0.62)], char=[(48, 22, 93, 86)], status="mask", note="horse's caparison (navy)"),
    "K_Ballista": dict(r=[R(205, 255, 0.4)], status="region", note="little blue on the machine"),
    "K_Catapult": dict(r=[R(205, 255, 0.4)], status="clear", note="blue frame parts"),
    "K_Gryphon": dict(r=[R(80, 170, 0.35)], status="owner", note="model texture is almost all team; the card's dragon is green"),
    "K_TransportShip": dict(r=[R(190, 235, 0.72, 0.0, 0.8)], char=[(20, 4, 88, 72)], status="mask", note="sails: blue near the sky backdrop's hue"),
    # ---- Wizard
    "W_Wizard": dict(r=[R(340, 18, 0.55)], status="clear", note="red robe"),
    "W_Wizard_F": dict(r=[R(270, 345, 0.25, 0.05, 0.75)], status="region", note="purple dress (backdrop is pink-red)"),
    "W_Engineer": dict(r=[R(330, 15, 0.45, 0.0, 0.7)], status="region", note="maroon scarf"),
    "W_Swordsman": dict(r=[R(335, 15, 0.45, 0.0, 0.75)], status="region", note="red loincloth"),
    "W_Crossbowman": dict(r=[R(215, 290, 0.25, 0.05, 0.7)], status="owner", note="dark bones; sprite team is the torso"),
    "W_Skeleknight": dict(r=[R(335, 15, 0.5, 0.0, 0.7)], status="region", note="maroon horse armour"),
    "W_Ballista": dict(r=[], status="owner", note="no team-coloured part in the art"),
    "W_Giant": dict(r=[], status="owner", note="model head texture is team; card shows grey-green skin"),
    "W_Dragon": dict(r=[R(340, 15, 0.6)], status="mask", note="red dragon on a pink-red backdrop"),
    "W_TransportShip": dict(r=[R(340, 15, 0.55)], status="clear", note="red sails"),
    # ---- Pirates
    "P_Captain": dict(r=[R(340, 15, 0.55, rect=[(0, 0, 60, 95)])], status="owner", note="coat and hat are index 0 (shown black); the parrot is red"),
    "P_Captain_F": dict(r=[R(140, 200, 0.25, 0.05, 0.6)], status="owner", note="dark green top"),
    "P_Builder": dict(r=[R(100, 180, 0.3)], status="clear", note="green hat and shirt stripes"),
    "P_Oarsman": dict(r=[R(345, 20, 0.45, 0.0, 0.7)], status="owner", note="dark red vest"),
    "P_Crossbowman": dict(r=[R(205, 250, 0.35)], status="clear", note="blue shirt"),
    "P_Horseman": dict(r=[R(345, 10, 0.6)], status="region", note="red cross on the shirt (horse is brown)"),
    "P_Barracuda": dict(r=[R(345, 12, 0.5)], status="clear", note="red sail stripes"),
    "P_Battleship": dict(r=[R(345, 12, 0.5)], status="clear", note="red sail stripes"),
    "P_FlyingDutchman": dict(r=[R(345, 12, 0.5)], status="clear", note="red sail stripes"),
    "P_TransportShip": dict(r=[R(345, 12, 0.5)], status="clear", note="red sail stripes"),
    # ---- Imperial
    "I_Governor": dict(r=[R(205, 250, 0.3, 0.0, 0.7)], status="region", note="navy coat (backdrop cyan)"),
    "I_Governor_F": dict(r=[R(180, 215, 0.3, 0.3, 1.0, rect=[(20, 30, 75, 95)])], status="owner", note="white dress, light blue lacing"),
    "I_Engineer": dict(r=[R(195, 240, 0.4, 0.0, 0.85)], status="mask", note="blue coat near the cyan backdrop"),
    "I_Swordsman": dict(r=[R(195, 240, 0.4, 0.0, 0.85)], status="mask", note="blue coat near the cyan backdrop"),
    "I_Rifleman": dict(r=[R(195, 240, 0.4, 0.0, 0.85)], status="mask", note="blue coat near the cyan backdrop"),
    "I_Cavalry": dict(r=[R(195, 240, 0.4, 0.0, 0.85)], status="mask", note="blue coat"),
    "I_Clipper": dict(r=[R(205, 245, 0.25, 0.0, 0.9)], status="mask", note="blue sail stripes"),
    "I_Battleship": dict(r=[R(205, 245, 0.25, 0.0, 0.9)], status="mask", note="blue sail stripes"),
    "I_Dirigible": dict(r=[R(205, 245, 0.25, 0.0, 0.9)], status="mask", note="blue sail stripes"),
    "I_TransportShip": dict(r=[R(205, 245, 0.25, 0.0, 0.9)], status="mask", note="blue sail stripes"),
    # ---- Earth (Mars)
    "E_Commander": dict(r=[R(2, 26, 0.6)], status="region", note="orange spacesuit (backdrop orange-yellow, skin yellow)"),
    "E_Commander_F": dict(r=[R(2, 26, 0.6)], status="region", note="orange spacesuit"),
    "E_Engineer": dict(r=[R(345, 15, 0.5)], status="region", note="red eye glow"),
    "E_Melee": dict(r=[R(350, 22, 0.6)], status="region", note="red emblem"),
    "E_Ranged": dict(r=[R(350, 22, 0.6)], status="region", note="red bits"),
    "E_Mounted": dict(r=[R(345, 15, 0.45)], status="region", note="red dome"),
    "E_SiegeOne": dict(r=[R(0, 25, 0.55)], status="region", note="orange/red hull parts"),
    "E_SiegeTwo": dict(r=[R(0, 25, 0.55)], status="region", note="orange/red hull parts"),
    "E_SiegeThree": dict(r=[R(0, 25, 0.55)], status="region", note="orange/red hull parts"),
    "E_TransportShip": dict(r=[R(0, 25, 0.55)], status="region", note="orange/red hull parts"),
    # ---- Alien
    "A_AlienKing": dict(r=[R(95, 170, 0.4)], status="clear", note="green arms and legs, torso panel"),
    "A_AlienKing_F": dict(r=[R(95, 170, 0.4)], status="clear", note="green limbs"),
    "A_Engineer": dict(r=[R(42, 75, 0.5, rect=[(20, 0, 78, 52)])], char=[(28, 2, 70, 50)], status="mask", note="yellow dome on a yellow-green backdrop"),
    "A_Melee": dict(r=[R(95, 170, 0.4)], status="clear", note="whole green body"),
    "A_Ranged": dict(r=[R(45, 70, 0.5)], status="region", note="yellow eye only"),
    "A_Mounted": dict(r=[R(42, 70, 0.5)], status="mask", note="yellow canopy"),
    "A_SiegeOne": dict(r=[R(55, 75, 0.7, 0.5)], status="mask", note="yellow-green trim near backdrop hue"),
    "A_SiegeTwo": dict(r=[R(55, 75, 0.7, 0.5)], status="mask", note="yellow-green tubes"),
    "A_SiegeThree": dict(r=[R(55, 75, 0.7, 0.5)], status="mask", note="yellow-green tubes"),
    "A_TransportShip": dict(r=[R(55, 75, 0.7, 0.5)], status="mask", note="yellow-green tubes"),
    # ---- Space police / criminals
    "SpacePoliceCaptain": dict(r=[R(350, 15, 0.55)], status="region", note="red helmet, red gun tip"),
    "SpacePolice": dict(r=[R(350, 15, 0.55)], status="region", note="red visor band, red jetpack"),
    "SpacePoliceShip": dict(r=[R(205, 245, 0.45)], status="clear", note="blue hull"),
    "SpaceCriminalLeader": dict(r=[R(3, 32, 0.55, ex=[(0, 55, 50, 95)])], status="region", note="orange head/arms/torso; excluding the fireball in his hand"),
    "SpaceCriminal": dict(r=[R(330, 15, 0.4, 0.0, 0.8)], status="clear", note="dark red jacket"),
    "SpaceCriminalShip": dict(r=[R(345, 15, 0.45)], status="region", note="red bits"),
    # ---- Neutral / creatures
    "Robot": dict(r=[R(10, 35, 0.6)], status="clear", note="orange wheels and parts (docs/437)"),
    "Monkey": dict(r=[R(10, 40, 0.35)], status="clear", note="brown fur (docs/437); the yellow hands stay"),
    "Wolf": dict(r=[R(0, 360, 0.0, 0.2, 1.0)], status="mask", note="grey body on a grey backdrop"),
    "Shark": dict(r=[R(0, 360, 0.0, 0.3, 1.0)], status="mask", note="grey body on a grey-blue backdrop"),
    # ---- Islanders / ninjas / dwarves / trolls
    "IslanderChief": dict(r=[R(345, 15, 0.5)], status="region", note="red mask (and the shield's red)"),
    "Islander": dict(r=[], status="owner", note="hat is index 0 (black); sprite team = hat and waist"),
    "IslanderWarCanoe": dict(r=[], status="owner", note="golem model almost all team; card is stone grey"),
    "NinjaKing": dict(r=[R(345, 18, 0.5)], status="clear", note="red robe"),
    "Ninja": dict(r=[R(235, 300, 0.15, 0.0, 0.6)], status="mask", note="dark purple suit (cf. docs/454 kHeadMasks ninja)"),
    "WarJunk": dict(r=[R(200, 245, 0.4)], status="mask", note="blue roof near the cyan backdrop"),
    "DwarfKing": dict(r=[], status="owner", note="sprite team = legs; not visible on the card"),
    "Dwarf": dict(r=[R(340, 18, 0.35, 0.0, 0.7)], status="region", note="maroon tunic"),
    "DwarfMineDefender": dict(r=[], status="owner", note="model texture team bits; card brown machine"),
    "TrollKing": dict(r=[], status="owner", note="dark armour; docs/454 tunic"),
    "Troll": dict(r=[R(8, 35, 0.5), R(120, 190, 0.15, 0.2, 0.9)], status="region", note="orange tunic + green skin (docs/454)"),
    "TrollBattleWheel": dict(r=[], status="owner", note="model texture team bits"),
    # ---- Heroes (orange backdrop)
    "ForestMan": dict(r=[R(80, 160, 0.35)], status="clear", note="green tunic"),
    "Ghost": dict(r=[], status="none", note="white sheet; sprite team is only the lantern"),
    "Sheriff": dict(r=[R(15, 40, 0.3, 0.1, 0.6, rect=[(0, 50, 95, 95)])], status="mask", note="brown suit, near the skin and the backdrop"),
    "Conquistador": dict(r=[R(90, 170, 0.35)], status="clear", note="green coat"),
    "AgentChase": dict(r=[R(200, 250, 0.3, 0.0, 0.6), R(60, 95, 0.5, rect=[(15, 40, 80, 95)])], status="region", note="navy jumpsuit + yellow-green trim"),
    "ClassicSpace": dict(r=[R(345, 15, 0.55)], status="clear", note="red suit and helmet"),
    "Santa": dict(r=[R(345, 15, 0.5)], status="clear", note="red suit and hat"),
}

# Buildings: one rule per faction (the roofs, flags, shields)
BUILDING_FACTION = {
    "K_": dict(r=[R(215, 250, 0.55)], note="blue roofs, shields, flags"),
    "I_": dict(r=[R(215, 250, 0.55)], note="blue roofs and flags"),
    "W_": dict(r=[R(345, 12, 0.7)], note="red roofs and banners"),
    "P_": dict(r=[R(345, 12, 0.6)], note="red roofs and sail cloth"),
    "E_": dict(r=[R(5, 32, 0.65)], note="orange domes and panels"),
    "A_": dict(r=[R(55, 72, 0.7, 0.55)], note="yellow-green tubes and lights"),
}
