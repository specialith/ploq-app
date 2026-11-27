// Ploq - make the signed update description (latest.json) for a release.
//
//   node tools/collab/installer/make_release.mjs Ploq-1.0.8.msi [Ploq-1.0.8-Setup.exe] [Ploq-1.0.8.dmg] [Ploq-1.0.8-x86_64.AppImage]
//        [--base URL] [--notes "text"]
//
// The Windows installer is required and is listed first; the Windows setup app (setup/build_setup.ps1:
// what ploq.app/download hands out on Windows), the macOS .dmg and the Linux AppImage are optional and
// must have the same version. Writes builds/installer/release/latest.json next to
// copies of the files. Upload them all, the installers first and latest.json last:
//   see tools/collab/installer/README.md ("Publishing an update").
//
// latest.json = {"signed": "<release JSON text>", "signature": "<base64 r||s>"}
// The app (src/collab/ploqupdate.cpp) checks the signature with Ploq's built-in public key. Windows
// then checks the downloaded .msi's size + SHA-256 against the signed text (it only ever takes the
// .msi: the update code picks the asset by suffix). macOS and Linux only read the version and open
// ploq.app/download, whose page (ploq-web) offers each platform's file from this list.
// The private key never goes into a repo:
//   %USERPROFILE%\.ploq\update-signing-key.pem  (override: PLOQ_UPDATE_KEY)

import { createHash, createPrivateKey, createPublicKey, sign, verify } from "node:crypto";
import { copyFileSync, mkdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { homedir } from "node:os";
import { basename, dirname, join } from "node:path";

const args = process.argv.slice(2);
const opt = (name, dflt) => {
    const i = args.indexOf(name);
    return i >= 0 ? args[i + 1] : dflt;
};
const files = args.filter((a, i) => !a.startsWith("--") && !args[i - 1]?.startsWith("--"));
const usage = 'usage: node make_release.mjs <Ploq-x.y.z.msi> [Ploq-x.y.z-Setup.exe] [Ploq-x.y.z.dmg] [Ploq-x.y.z-x86_64.AppImage] [--base URL] [--notes "text"]';
if (files.length === 0) {
    console.error(usage);
    process.exit(2);
}

// Exact names only: the download page and the apps match on them.
const SHAPES = [
    { kind: "Windows installer", re: /^Ploq-(\d+\.\d+\.\d+)\.msi$/ },
    { kind: "Windows setup", re: /^Ploq-(\d+\.\d+\.\d+)-Setup\.exe$/ },
    { kind: "macOS disk image", re: /^Ploq-(\d+\.\d+\.\d+)\.dmg$/ },
    { kind: "Linux AppImage", re: /^Ploq-(\d+\.\d+\.\d+)-x86_64\.AppImage$/ },
];
const found = files.map((path) => {
    const name = basename(path);
    const shape = SHAPES.findIndex((s) => s.re.test(name));
    if (shape < 0) {
        throw new Error(`unexpected file name ${name}\n${usage}`);
    }
    return { path, name, shape, version: SHAPES[shape].re.exec(name)[1] };
});
if (!found.some((f) => f.shape === 0)) {
    throw new Error("the Windows installer (Ploq-x.y.z.msi) is required: Windows copies update from this feed");
}
if (new Set(found.map((f) => f.shape)).size !== found.length) {
    throw new Error("one file of each kind");
}
const version = found[0].version;
if (found.some((f) => f.version !== version)) {
    throw new Error(`all files must have the same version: ${found.map((f) => f.name).join(", ")}`);
}
found.sort((a, b) => a.shape - b.shape); // .msi first: older download pages take assets[0]

const base = opt("--base", "https://collab.ploq.app/updates/files").replace(/\/+$/, "");
const notes = opt("--notes", `Ploq ${version}`);

const release = {
    product: "ploq",
    tag_name: `v${version}`,
    bodyMarkdown: notes,
    published: new Date().toISOString(),
    assets: found.map((f) => ({
        name: f.name,
        browser_download_url: `${base}/${f.name}`,
        size: statSync(f.path).size,
        sha256: createHash("sha256").update(readFileSync(f.path)).digest("hex"),
    })),
};
const signedText = JSON.stringify(release);

const keyPath = process.env.PLOQ_UPDATE_KEY || join(homedir(), ".ploq", "update-signing-key.pem");
const privateKey = createPrivateKey(readFileSync(keyPath));
const signature = sign("sha256", Buffer.from(signedText, "utf8"), { key: privateKey, dsaEncoding: "ieee-p1363" });
// Self-check with the public half before publishing anything.
if (!verify("sha256", Buffer.from(signedText, "utf8"), { key: createPublicKey(privateKey), dsaEncoding: "ieee-p1363" }, signature)) {
    throw new Error("signature self-check failed");
}

const outDir = join(dirname(found[0].path), "release");
mkdirSync(outDir, { recursive: true });
writeFileSync(join(outDir, "latest.json"), JSON.stringify({ signed: signedText, signature: signature.toString("base64") }, null, 1));
for (const f of found) {
    copyFileSync(f.path, join(outDir, f.name));
}
console.log(`Ploq ${version}: ${join(outDir, "latest.json")} + ${found.map((f) => f.name).join(", ")}`);
for (const a of release.assets) {
    console.log(`  ${a.name}  sha256 ${a.sha256}  size ${a.size}  -> ${a.browser_download_url}`);
}
