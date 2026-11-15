# Behaviour spec distilled from scripts.txt (port reference)

Units: stage 480x360, origin centre, y up. f = dt*30. "Scratch frame" = 1/30 s grid for waits:
wait(d) lasts max(1, ceil(d*30-1e-6)) frames of 1/30 s (accumulate real time). Non-warp loops = 1 iteration/frame.

## Projection / sizes
D=300. rel=obj-cam (cam Z=0). relZ>30 visible: pos=rel.xy*D/relZ, size%=100*D/relZ.
Size clamp per costume (w,h = svg width/height): min=min(1,max(5/w,5/h)), max=min(720/w,540/h).
Fence (from "0.1" costume 2.5x2.5, rc (1.4077,1.3165), size 15429%): circle r=192.9 centred at
(x-24.3, y+10.3) must overlap stage inset 15 => x in [-393.6, 442.2], y in [-368.2, 347.6].
PORT: fence and max-size clamp are intentionally NOT replicated for world objects (Alfie, 2026-10-27): objects
leave the screen edges freely and keep growing; only the min-size clamp is kept. Draw list culls objects whose
bbox is outside +-1000 x +-600 stage units (never visible on any viewport).
Draw order: far->near by squared distance.

## Objects (Stars clones), per frame order: delete if OZ<100; then by type:
- Power up: hue += 1*f; OZ -= speed*f; if |X-OX|<250,|Y-OY|<250,OZ<250: (if playing: ammo+1, score+2, sfx pop) delete.
- Bullet: hue += 10*f; if OZ>4000 {B=(0,0,-250); delete}; OX=X+sin(6000t)sin(30t)*20; OY=Y+cos(6000t)cos(30t)*20 (deg);
  OZ += 60f; B=(OX,OY,OZ) (one global, last writer wins).
- Stars/planets: OZ -= speed*f. Planets (Earth, Planet2): box 250 & OZ<250 => Lose.
  |BX-OX|<275,|BY-OY|<275, OZ<BZ+275 => score+1, sfx Hit, delete planet.
- All clones fade in ghost 100->0 over 10 Scratch frames (0.333 s).
## Spawners (from Title, paused when Pause; killed by Stop music)
- Stars: loop {add 2 (rand(-1000,1000)+X, rand+Y, 8000) with current costume; costume=rand(1,2) ("0.1" or "1");
  wait 0.25-(N*(speed/60)/150)*0.125; 1/10: costume "0.01"}. N slider 0..300 default 250.
- Power ups: loop { add at (rand+X, rand+Y, 8000); wait 1 }.
- Planets (Start game): normal: loop{ wait rand(0,1) int; repeat rand(1,5): add rand(Earth,Planet2) at Z=speed*100, one per frame}.
  hard: every frame where >0.025 s since last (30 Hz grid => every frame): add planet; speed=80; N=0 (sticky).
- Shoot (Start game): B=(0,0,-250). loop { wait until (space or Shoot!) and ammo>0 and !pause; Shoot!=false; add bullet at
  (X,Y,Z+100); sfx Crunch; ammo-1; wait 1 }.
## Ship (Start game, Sprite3)
keyboard: X += (R-L)*10f, Y += (U-D)*10f. mouse: v=(Target-pos)/10 clamped +-10, pos += v*f.
speed += round((E-Q)*10*f)/50, clamp [5,80], start 60.
Cursor: on press => mouse mode, show dots, show Pause button ("Mobile detected"); remember mouse+Target;
while held Target=old+(mouse-oldmouse)*3; release with |d|<10 both axes => Shoot!=true.
Any WASD/arrow => keyboard mode, Target=pos, hide dots, hide pause button ("PC detected").
Dots: IDs 1..8 and 10, Dot costume 10% size, ghost 90, at ((Target-pos)/10)*ID/5. Start game: Target=0, dots visible.
## Title (Title broadcast)
Start Screen=1, mode sel false, speed 60, Score=0 (WR sprite), Timer=0, hide HUD, show N slider, Music=1.
Title logo + overlay(black ghost 50) ; logo fades in 50 frames (wait .01 x50 => 1.67 s), clones (buttons) too.
Buttons: Play(0,-15) Select mode(135,-15) What's new?(-135,-15) Toggle Music(180,-120) [Toggle Multiplayer(-160,-120): omitted]
Description(0,0). Hover size -> 120 else 100, step (t-s)/5*f. Mode sel: costume id+1 (Back/Hard mode/Normal mode), y->45,
step (t-y)/3*f; Description y->-40 (else 0) showing 'normal'/'hard' (hover Normal/Hard sets it; initial 'hard').
[SELECTED] created at click on Select mode at (0,-5), size = Select mode button's size then, x->+-135 ((t-x)/3*f), mode
normal=-135 hard=+135, deleted when mode sel ends. Click (press on hovered): sfx pop; Play: start; Select mode: mode sel;
What's new?: Changelog overlay at (0,0) size 100 front, close on press+release of any key/mouse; Normal/Hard: set Mode;
Back: leave mode sel; Toggle Music: PlayerMusic toggle (off => stop all sounds). Toggle Music colour effect = PM*50.
Camera: X += (cursor-X)/10*f on title. Space (after release) or Play => Play game:
fade everything out 1 s (ghost +3.33/frame), camera eases to 0 ((0-X)/10 f); then Ammo=5, show HUD, Start Screen=0,
Shoot!=false, Start game.
Stage bg on title: colour effect +1 per frame (+2 on first title: 2 scripts) => bg = HSV(e/200,1,0.055) (Scratch
minLightness rule on black). Cleared when Start Screen=0.
## Lose / stop music
Lose: sfx Pop; Stop music (Music=0, stop all sounds, delete all clones, stop spawners/ship/WR scripts, update WR with
current if greater); wait 1; if PlayerMusic: sfx Lose, wait 3; Title.
Easter egg: on Up press, wait until Down, Left, Right, b, a held (each in turn) => Stop music, wait 1, play Easter egg
(60.8 s) until done, wait 1, Title. Cheat: q then w,e,r,t,y held in turn => timer +1 extra per second forever.
## Timer / records
Timer counts while Music=1 and Start Screen=0, frozen when paused. WR timer 687.19, WR score 1348 (cloud values) or
saved local bests. WR beaten popup "You beat the world record!" at (0,100), after 1 s into game wait until
Timer>WR (sep. Score>WR): pop sfx, WR=value, 100 steps of 0.05 s: ghost+1, y+0.5; then WR tracks value every 5 s.
## Pause (Enter/P/click pause button; only Start Screen=0)
Toggle; world frozen (no render-frame logic, no spawns, no ship). Panel costume1 at (0,0) front. Hides HUD, shows N slider.
Pause button costume2 (bounds x -240.5..-133.5, y -95.5..-152.5) shown in mouse mode, hidden after unpause.
Sfx pop on toggle.
## Music
Stage stream: Intro (once at boot if PM) then loop Main. Thumbnail stream: Options intro / Options select in lockstep.
Stage vol -> 100 unless (mode sel or pause) -> 0; thumb opposite; step (t-v)/10*f. Thumb starts at 100 at boot.
Stop all sounds stops both + sfx. After Stop music, Title restarts at Main/Options select. Key 1 toggles PM (PC).
Sound volume also applies to that target's sfx (stage: Pop, Lose, Easter egg).
## Monitors (Scratch style, positions = top-left in stage px)
Play: Ship speed: (5,5), Ammo: (0,332), ☁ World record score: (156,336), Score: (201,317), Timer: (376,34),
☁ World record timer: (288,0). Title/pause: slider "Number of stars (turn down if you're laggy)" (79,236), 0..300.
FPS: (385,330) always visible. Values formatted like JS (toFixed(6) trimmed).
Style: bg hsla(215,100%,95%) #e5f0ff, border 1px rgba(0,0,0,.15), radius 4, padding 3; label bold 12px #575e75;
value pill #ff8c1a white text, min-width 40, radius 4, margin 0 5px.

## Port-only option: 3D colour shimmer (3DS, 2026-10-31)
Off by default. Bottom-screen toggle "3D colour shimmer  Off/On" (slider-monitor style) on the title (under the
buttons, hidden in mode select/changelog) and on the pause screen (above Resume/Quit). Touch only, acts on release.
When on and the 3D slider is up, the right eye draws CO_BULLET and CO_POWERUP (the only hue-cycling projectiles;
the game has no enemy bullets, planets don't shoot) with hue + 100 (half a turn, Scratch units 200 = 1 turn); the
left eye keeps the normal hue, so their usual cycling continues with complementary colours per eye. Slider at 0:
only the left eye is drawn, normal colours. Saved as a 4th int in records.txt ("wr_timer wr_score music shimmer");
old 3-field files load as off, old builds ignore the extra field. PC build unchanged (no stereo).
Screenshot note: right after a shot the bullet is still fading in (ghost 100->0 over 0.33 s), so planets behind it
show through and it can look two-tone in one eye. That happens with the option off too (same in both eyes then).
