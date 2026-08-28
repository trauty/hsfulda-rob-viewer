# ROS-/MCAP-Punktwolken auf Android anzeigen

Möglichkeiten, 3D-Punktwolken vom Roboter auf einem Android-Gerät anzusehen, entweder gespeicherte MCAP-Dateien oder live.

---

## Browser-basiert

### [Foxglove Web](https://github.com/foxglove/studio) (Chrome)

**Pro**
- Natives MCAP, sehr gutes 3D-Panel, live + aufgezeichnet
- Keine Installation auf dem Handy nötig

**Contra**
- Desktop UI: auf dem Handy basically unusable, auf dem Tablet nicht viel besser
- Nur Chrome, Account nötig, Closed Source

### [Lichtblick](https://github.com/lichtblick-suite/lichtblick) (Open-Source-Fork von Foxglove)

**Pro**
- Gleicher Funktionsumfang, self-host, kein Account

**Contra**
- Gleiche Probleme mit der Bedienung wie Foxglove Web
- Hinkt Upstream hinterher

### [ROSboard](https://github.com/dheera/rosboard)

**Pro**
- Für Handys gebaut, läuft als einzelne ROS-Node
- Subsampling auf dem Roboter, übersteht schlechtes WLAN

**Contra**
- Punktwolkenansicht ist unausgebildet
- Nur live, keine Wiedergabe von MCAP

### [Rerun Web-Viewer](https://github.com/rerun-io/rerun)

**Pro**
- Schnellster Renderer (webgpu), Open Source
- MCAP + reflection basiertes ROS2 (Custom-Messages funktionieren damit)

**Requirements**
- WebGPU braucht Android 12+

**Contra**
- MCAP noch experimentell, lädt Recording komplett in den RAM, kein Streaming

---

## Remote-Rendering

### [Sunshine + Moonlight](https://github.com/lizardbyte/sunshine) (Pixel-Streaming)

**Pro**
- Performance unabhängig von der Punktzahl
- Volles rviz2/Foxglove

**Contra**
- Braucht GPU + X-Session irgendwo
- Fernsteuerung per Mauszeiger, fühlt sich nicht wie eine App an

### [web_video_server](https://github.com/RobotWebTools/web_video_server) (rviz-Kameraansicht)

**Pro**
- Lightweight

**Contra**
- Feste Kamera, kein Drehen/Zoomen
- MJPEG-Qualität könnte nicht für feine Details reichen

---

## Nativ Android

### [AR-RViz](https://github.com/kodie-artner/AR-RViz)

**Pro**
- Voll nativ, AR-Overlay auf den echten Roboter
- ROS1 + ROS2 über ROS-TCP-Endpoint

**Contra**
- Roboter muss im `map`-Frame lokalisiert sein
- Wird nicht weiterentwickelt
- Wurde mit Unity gemacht

### [cogniteam/android_ros_pointcloud_viewer](https://github.com/cogniteam/android_ros_pointcloud_viewer)

**Pro**
- Der Name ist Programm

**Contra**
- Unmaintained, ROS1, selbst bauen und reparieren

---

## Offline

### MCAP → PLY/LAS oder → [Potree-Octree](https://github.com/potree/potree)

**Pro**
- Potree streamt riesige Wolken flüssig auf Mobilgeräten
- Keine Verbindung zum Roboter nötig

**Contra**
- Nie live, ROS-Kontext geht verloren
- Generische Android-LAS/PLY-Viewer sind schwach

### Linux auf dem Handy (Termux / Terminal-VM + rviz2)

**Pro**
- Wäre Full-Fledged

**Contra**
- Software Rendering, sehr langsam
- Nerviges Setup, schlechte Akkulaufzeit und Temperaturen
