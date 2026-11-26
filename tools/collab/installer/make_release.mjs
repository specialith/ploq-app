// Ploq - make the signed update description (latest.json) for an installer.
//
//   node tools/collab/installer/make_release.mjs builds/installer/Ploq-1.0.1.msi [--base URL] [--notes "text"]
//
// Writes builds/installer/release/latest.json next to a copy of the .msi. Upload both:
//   see tools/collab/installer/README.md ("Publishing an update").
//
// latest.json = {"signed": "<release JSON text>", "signature": "<base64 r||s>"}
// The app (src/collab/ploqupdate.cpp) checks the signature with Ploq's
// built-in public key, then checks the downloaded installer's size + SHA-256
// against the signed text. The private key never goes into a repo:
//   %USERPROFILE%\.ploq\update-signing-key.pem  (override: PLOQ_UPDATE_KEY)

import { createHash, createPrivateKey, createPublicKey, sign, verify } from "node:crypto";
import { copyFileSync, mkdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { homedir } from "node:os";
import { basename, dirname, join } from "node:path";

const args = process.argv.slice(2);
const msiPath = args.find((a) => !a.startsWith("--") && args[args.indexOf(a) - 1]?.startsWith("--") !== true);
const opt = (name, dflt) => {
    const i = args.indexOf(name);
    return i >= 0 ? args[i + 1] : dflt;
};
if (!msiPath) {
    console.error("usage: node make_release.mjs <Ploq-x.y.z.msi> [--base https://collab.ploq.app/updates/files] [--notes text]");
    process.exit(2);
}

const name = basename(msiPath);
const m = /^Ploq-(\d+\.\d+\.\d+)\.msi$/.exec(name);
if (!m) {
    throw new Error(`installer must be named Ploq-<x.y.z>.msi, got ${name}`);
}
const version = m[1];
const base = opt("--base", "https://collab.ploq.app/updates/files").replace(/\/+$/, "");
const notes = opt("--notes", `Ploq ${version}`);

const data = readFileSync(msiPath);
const release = {
    product: "ploq",
    tag_name: `v${version}`,
    bodyMarkdown: notes,
    published: new Date().toISOString(),
    assets: [{
        name,
        browser_download_url: `${base}/${name}`,
        size: statSync(msiPath).size,
        sha256: createHash("sha256").update(data).digest("hex"),
    }],
};
const signedText = JSON.stringify(release);

const keyPath = process.env.PLOQ_UPDATE_KEY || join(homedir(), ".ploq", "update-signing-key.pem");
const privateKey = createPrivateKey(readFileSync(keyPath));
const signature = sign("sha256", Buffer.from(signedText, "utf8"), { key: privateKey, dsaEncoding: "ieee-p1363" });
// Self-check with the public half before publishing anything.
if (!verify("sha256", Buffer.from(signedText, "utf8"), { key: createPublicKey(privateKey), dsaEncoding: "ieee-p1363" }, signature)) {
    throw new Error("signature self-check failed");
}

const outDir = join(dirname(msiPath), "release");
mkdirSync(outDir, { recursive: true });
writeFileSync(join(outDir, "latest.json"), JSON.stringify({ signed: signedText, signature: signature.toString("base64") }, null, 1));
copyFileSync(msiPath, join(outDir, name));
console.log(`Ploq ${version}: ${outDir}\\latest.json + ${name}`);
console.log(`  sha256 ${release.assets[0].sha256}  size ${release.assets[0].size}`);
console.log(`  download URL ${release.assets[0].browser_download_url}`);
