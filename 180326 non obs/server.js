// UPDATED SERVER.JS – FINAL + AUTO DELETE OLD GALLERY (7 DAYS)
const express = require("express");
const fs = require("fs");
const path = require("path");
const multer = require("multer");

const app = express();
const PORT = 3001;
const BASE_DIR = __dirname;
app.use(express.json());
app.use(express.raw({ type: "image/jpeg", limit: "30mb" }));

// ===================== CONFIG AUTO DELETE =====================
const MAX_GALLERY_AGE_DAYS = 7; 
const CHECK_INTERVAL_MS = 3600000; // Cek setiap 1 jam
const OFFLINE_TIMEOUT_MS = 120000; // 2 menit tanpa upload/gallery/sdinfo = offline

// ===================== SETTINGS FILE SETUP =====================
const SETTINGS_FILE = path.join(BASE_DIR, "camera_settings.json");

let savedSettings = {};
if (fs.existsSync(SETTINGS_FILE)) {
  try {
    savedSettings = JSON.parse(fs.readFileSync(SETTINGS_FILE));
    console.log("Settingan dimuat:", Object.keys(savedSettings));
  } catch (e) {
    console.log("Gagal memuat settingan, pakai default.");
  }
}

function saveSettingsToFile() {
  fs.writeFileSync(SETTINGS_FILE, JSON.stringify(savedSettings, null, 2));
}

function updateSavedConfig(id, key, value) {
    if (!savedSettings[id]) savedSettings[id] = {};
    savedSettings[id][key] = value;
    saveSettingsToFile();
}

// ===================== DATA STRUCTURES =====================
const cameras = {}; 

function getCam(id) {
  if (!cameras[id]) {
    const saved = savedSettings[id] || {};
    cameras[id] = {
      shouldRun: false,
      streamFps: saved.streamFps || 5, 
      thumbIntervalMs: saved.thumbIntervalMs || 60000,
      galleryIntervalMs: saved.galleryIntervalMs || 300000,
      viewers: 0,
      sdInfo: { totalMB: 0, usedMB: 0, freeMB: 0 }, 
      bw: {
        bytesThisSecond: 0,
        bytesPerSecond: 0,
        bytesSession: 0,
        samples: []
      },
      alias: saved.alias || null,
      currentVersion: saved.currentVersion || null,
      status: "offline",
      lastSeenTs: null,
      lastUploadTs: null,
      lastGalleryTs: null,
      lastSdInfoTs: null
    };
    fs.mkdirSync(path.join(BASE_DIR, `frames/${id}`), { recursive: true });
    fs.mkdirSync(path.join(BASE_DIR, `gallery/${id}`), { recursive: true });
  }
  return cameras[id];
}

function updatePresence(cam, type) {
  const now = Date.now();
  cam.lastSeenTs = now;
  cam.status = "online";
  if (type === "upload") cam.lastUploadTs = now;
  if (type === "gallery") cam.lastGalleryTs = now;
  if (type === "sdinfo") cam.lastSdInfoTs = now;
}

function getCameraSummary(id) {
  const cam = getCam(id);
  const lastSeenTs = cam.lastSeenTs || 0;
  const isOnline = lastSeenTs > 0 && (Date.now() - lastSeenTs) <= OFFLINE_TIMEOUT_MS;
  cam.status = isOnline ? "online" : "offline";
  return {
    id,
    name: cam.alias || id,
    alias: cam.alias || null,
    status: cam.status,
    lastSeenTs: cam.lastSeenTs,
    currentVersion: cam.currentVersion || null,
    hasThumbnail: fs.existsSync(path.join(BASE_DIR, `frames/${id}/latest.jpg`)),
    viewers: cam.viewers || 0,
    shouldRun: !!cam.shouldRun
  };
}

// ===================== FOLDERS =====================
["frames", "gallery", "firmware"].forEach(d => {
  const dirPath = path.join(BASE_DIR, d);
  if (!fs.existsSync(dirPath)) fs.mkdirSync(dirPath);
});

// ===================== AUTO CLEANUP LOGIC (NEW) =====================
function autoCleanupGallery() {
    console.log("[CLEANUP] Mengecek file gallery lama...");
    const galleryRoot = path.join(__dirname, "gallery");
    const now = Date.now();
    const maxAgeMs = MAX_GALLERY_AGE_DAYS * 24 * 60 * 60 * 1000;
    
    // 1. Baca semua folder kamera
    if (fs.existsSync(galleryRoot)) {
        const camFolders = fs.readdirSync(galleryRoot);
        
        camFolders.forEach(camId => {
            const camDir = path.join(galleryRoot, camId);
            // Pastikan itu folder
            if (fs.lstatSync(camDir).isDirectory()) {
                const files = fs.readdirSync(camDir);
                
                files.forEach(file => {
                    const filePath = path.join(camDir, file);
                    try {
                        const stats = fs.statSync(filePath);
                        // 2. Hitung umur file
                        const fileAge = now - stats.mtimeMs; 
                        
                        // 3. Hapus jika kadaluarsa
                        if (fileAge > maxAgeMs) {
                            fs.unlinkSync(filePath);
                            console.log(`[DELETE] Menghapus file lama: ${camId}/${file}`);
                        }
                    } catch (err) {
                        console.error(`[ERROR] Gagal hapus ${file}:`, err.message);
                    }
                });
            }
        });
    }
}

// Jalankan cleanup saat server start, lalu ulangi tiap 1 jam
autoCleanupGallery(); 
setInterval(autoCleanupGallery, CHECK_INTERVAL_MS);


// ===================== SD CARD INFO =====================
app.post("/sdinfo", (req, res) => {
  const camId = req.query.cam_id;
  if (!camId) return res.status(400).send("No ID");
  const cam = getCam(camId);
  if (req.body) cam.sdInfo = req.body;
  updatePresence(cam, "sdinfo");
  console.log(`[SDINFO] ${camId}`, cam.sdInfo);
  res.send("OK");
});

// ===================== FRAME UPLOAD =====================
app.post("/upload", (req, res) => {
  const camId = req.query.cam_id;
  if (!camId || !req.body || req.body.length < 10) return res.sendStatus(400);

  const cam = getCam(camId);
  const isFirstFrame = !fs.existsSync(path.join(BASE_DIR, `frames/${camId}/latest.jpg`));
  fs.writeFileSync(path.join(BASE_DIR, `frames/${camId}/latest.jpg`), req.body);
  updatePresence(cam, "upload");

  const len = req.body.length;
  cam.bw.bytesThisSecond += len;
  cam.bw.bytesSession += len;
  cam.bw.samples.push({ ts: Math.floor(Date.now() / 1000), bytes: len });

  if (isFirstFrame) console.log(`[UPLOAD] First frame received from ${camId} (${len} bytes)`);
  res.send("OK");
});

// ===================== GALLERY UPLOAD =====================
app.post("/galleryUpload", (req, res) => {
  const camId = req.query.cam_id;
  if (!camId) return res.sendStatus(400);
  const cam = getCam(camId);
  const name = `gallery-${Date.now()}.jpg`;
  fs.writeFileSync(path.join(BASE_DIR, `gallery/${camId}/${name}`), req.body);
  updatePresence(cam, "gallery");
  console.log(`[GALLERY] Saved ${camId}/${name}`);
  res.send("OK");
});

// List Gallery per kamera
app.get("/gallery/:cam", (req, res) => {
  const dir = path.join(BASE_DIR, `gallery/${req.params.cam}`);
  if (!fs.existsSync(dir)) return res.json([]);
  res.json(fs.readdirSync(dir).reverse());
});

// ===================== THUMBNAIL =====================
app.get("/thumbnail/:cam", (req, res) => {
  const f = path.join(BASE_DIR, `frames/${req.params.cam}/latest.jpg`);
  if (!fs.existsSync(f)) return res.sendStatus(404);
  res.setHeader("Content-Type", "image/jpeg");
  res.send(fs.readFileSync(f));
});

// ===================== STREAM =====================
app.get("/stream/:cam", (req, res) => {
  const camId = req.params.cam;
  const cam = getCam(camId);
  cam.viewers++;
  cam.shouldRun = true;
  cam.bw.bytesSession = 0;

  res.writeHead(200, { "Content-Type": "multipart/x-mixed-replace; boundary=frame" });

  const iv = setInterval(() => {
    const f = path.join(BASE_DIR, `frames/${camId}/latest.jpg`);
    if (!fs.existsSync(f)) return;
    const frame = fs.readFileSync(f);
    res.write("--frame\r\nContent-Type: image/jpeg\r\n\r\n");
    res.write(frame);
    res.write("\r\n");
  }, 1000 / cam.streamFps);

  req.on("close", () => {
    clearInterval(iv);
    cam.viewers--;
    if (cam.viewers <= 0) cam.shouldRun = false;
  });
});

// ===================== STATUS & CONFIG =====================
app.get("/status", (req, res) => {
  const camId = req.query.cam_id;
  if (!camId) return res.sendStatus(400);
  const cam = getCam(camId);
  res.json({
    run: cam.shouldRun,
    streamFps: cam.streamFps,
    thumbIntervalMs: cam.thumbIntervalMs,
    galleryIntervalMs: cam.galleryIntervalMs
  });
});

app.post("/cam/:cam/setFps", (req, res) => {
  const id = req.params.cam; const cam = getCam(id);
  const val = Number(req.body.fps);
  if(val) { cam.streamFps = val; updateSavedConfig(id, 'streamFps', val); }
  res.json({ streamFps: cam.streamFps });
});

app.post("/cam/:cam/setThumbInterval", (req, res) => {
  const id = req.params.cam; const cam = getCam(id);
  const val = Number(req.body.ms);
  if(val !== undefined) { cam.thumbIntervalMs = val; updateSavedConfig(id, 'thumbIntervalMs', val); }
  res.json({ thumbIntervalMs: cam.thumbIntervalMs });
});

app.post("/cam/:cam/setGalleryInterval", (req, res) => {
  const id = req.params.cam; const cam = getCam(id);
  const val = Number(req.body.ms);
  if(val !== undefined) { cam.galleryIntervalMs = val; updateSavedConfig(id, 'galleryIntervalMs', val); }
  res.json({ galleryIntervalMs: cam.galleryIntervalMs });
});

app.post("/cam/:cam/start", (req, res) => { getCam(req.params.cam).shouldRun = true; res.send("OK"); });
app.post("/cam/:cam/stop", (req, res) => { getCam(req.params.cam).shouldRun = false; res.send("OK"); });

// ===================== DATA POLLING =====================
app.get("/cameras", (req, res) => {
  const activeCams = Object.keys(cameras);
  const savedCams = Object.keys(savedSettings);
  res.json([...new Set([...activeCams, ...savedCams])]);
});

app.get("/api/cameras", (req, res) => {
  const activeCams = Object.keys(cameras);
  const savedCams = Object.keys(savedSettings);
  const allIds = [...new Set([...activeCams, ...savedCams])];
  const items = allIds.map(getCameraSummary).sort((a, b) => {
    if (a.status !== b.status) return a.status === "online" ? -1 : 1;
    return (a.name || a.id).localeCompare(b.name || b.id);
  });
  res.json(items);
});

setInterval(() => {
  Object.values(cameras).forEach(cam => {
    cam.bw.bytesPerSecond = cam.bw.bytesThisSecond;
    cam.bw.bytesThisSecond = 0;
    if (cam.lastSeenTs && (Date.now() - cam.lastSeenTs) > OFFLINE_TIMEOUT_MS) {
      cam.status = "offline";
    }
  });
}, 1000);

app.get("/bandwidth/:cam", (req, res) => {
  const cam = getCam(req.params.cam); 
  if (!cam) return res.sendStatus(404);
  const now = Math.floor(Date.now() / 1000);
  const last24 = cam.bw.samples.filter(s => s.ts > now - 86400).reduce((a, b) => a + b.bytes, 0);
  res.json({
    kbps: (cam.bw.bytesPerSecond / 1024).toFixed(2),
    sessionMB: (cam.bw.bytesSession / 1024 / 1024).toFixed(2),
    total24MB: (last24 / 1024 / 1024).toFixed(2),
    sd: cam.sdInfo 
  });
});

// ===================== OTA =====================
const uploadFW = multer({ dest: path.join(BASE_DIR, "firmware") });
app.post("/uploadFirmware", uploadFW.single("firmware"), (req, res) => {
  const fp = path.join(BASE_DIR, "firmware/firmware.bin");
  if (fs.existsSync(fp)) fs.unlinkSync(fp);
  fs.renameSync(req.file.path, fp);
  fs.writeFileSync(path.join(BASE_DIR, "firmware/version.json"), JSON.stringify({ version: Date.now(), url: "/firmware.bin" }));
  res.send("OK");
});
app.get("/firmware.bin", (req, res) => res.sendFile(path.join(BASE_DIR, "firmware/firmware.bin")));
app.get("/checkUpdate", (req, res) => res.json(JSON.parse(fs.readFileSync(path.join(BASE_DIR, "firmware/version.json")))));

// ===================== RUN =====================
app.use("/gallery", express.static(path.join(BASE_DIR, "gallery")));
app.get("/", (req, res) => { res.sendFile(path.join(BASE_DIR, "public/index.html")); });

app.listen(PORT, () => console.log(`SERVER READY PORT ${PORT} (Auto-Cleanup Active)`));
