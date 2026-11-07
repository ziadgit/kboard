# BTS Wand display
Gestures from the wand show BTS content full screen.
- SHAKE: random official BTS video
- TILT_LEFT / TILT_RIGHT: photo slideshow
- POINT_UP: latest posts from @bts_bighit on X (snapshot)
- TWIST: random member spotlight
- WAVE: fun fact

Run:
    pip3 install -r requirements.txt
    python3 wand_bridge.py          # real wand over Bluetooth
    python3 wand_bridge.py --fake   # no wand: type gestures in the terminal
The page opens at http://localhost:8000. Keyboard fallback: S, arrows, T, W.
Photos are BIGHIT/HYBE promo images: fine for a local demo, not openly licensed.
