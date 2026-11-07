# BTS content handoff

Verified catalog in `bts_content.json`: 21 official-channel videos, 16 images (8 official profile photographs + 8 MV thumbnails), 16 sourced facts, 7 member profiles, 6 recent official X posts with engagement metrics.

Schema: videos[{id,title,category,author,url,verification_url,thumbnail_url}], images[{id,title,url,file,source_url,credit,license,member?}], members[{name,role,birthday,photo,source_url,role_source_url}], facts[{text,source_url}], posts[{text,author,url,date,timezone,metrics}]. `file` and `photo` paths are relative to this directory.

All member birthdays/facts derive from https://bts.ibighit.com/eng/profile/ ; roles from https://commons.wikimedia.org/wiki/Category:BTS . Member roles use broad descriptors rather than contestable main/lead ranks.

Images are copyrighted official publicity/thumbnail material, NOT freely licensed Commons images. Obtain rights before redistribution or public hosting. Commons API access returned HTTP 403. Local images were decoded/verified. YouTube oEmbed confirms existence and channel identity, NOT embeddability or playback availability.

No app or BLE bridge is implemented yet. The code-changes skill mandates CloudAgent for nontrivial app coding; no CloudAgent tool is available to this executor. Parent should use its CloudAgent tool if available, supplying this JSON and media. Intended app: purple full-screen localhost:8000, websocket localhost:8765; SHAKE random video, tilt photos, POINT_UP X, TWIST member, WAVE fact. Keyboard S/arrows/T/W. BLE NUS KpopWand, TX 6E400003-B5A3-F393-E0A9-E50E24DCCA9E, RX 6E400002-B5A3-F393-E0A9-E50E24DCCA9E, response F:A020F0:2 newline. Include a user-click Enable playback control because BLE events cannot guarantee audible autoplay.
