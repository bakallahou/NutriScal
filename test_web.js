// Tests Node de la plateforme web (logique + cablage de l'interface avec faux DOM / faux Firebase).
// Ne remplace PAS un test dans un vrai navigateur avec un vrai projet Firebase.
const fs = require('fs'), vm = require('vm'), assert = require('assert'), path = require('path');
const html = fs.readFileSync(path.join(__dirname, '..', 'web', 'index.html'), 'utf8');
const logic = /<script id="ns-logic">([\s\S]*?)<\/script>/.exec(html)[1];
const appSrc = /<script type="module" id="ns-app">([\s\S]*?)<\/script>/.exec(html)[1];
let n = 0; const ok = (c, m) => { n++; assert.ok(c, m); };
const eq = (a, b, m) => { n++; assert.deepStrictEqual(a, b, m); };

// ---------- 1. calculs ----------
const mod = { exports: {} }; vm.runInNewContext(logic, { module: mod, globalThis: {} }); const NS = mod.exports;
let r = NS.calcTargets({ sexe: 'H', age: 30, poids: 80, taille: 180, activite: 'modere', objectif: 'masse' });
eq([r.bmr, r.tdee, r.kcal, r.protein, r.fat, r.carbs], [1780, 2759, 3035, 160, 80, 419], 'homme 80kg masse');
r = NS.calcTargets({ sexe: 'F', age: 28, poids: 60, taille: 165, activite: 'leger', objectif: 'seche' });
eq([r.bmr, r.kcal, r.protein, r.fat, r.carbs], [1330, 1463, 132, 48, 126], 'femme seche');
r = NS.calcTargets({ sexe: 'H', age: 16, poids: 65, taille: 175, activite: 'modere', objectif: 'seche' });
ok(r.kcal === r.tdee && r.notes.length === 1, 'mineur : pas de deficit');
r = NS.calcTargets({ sexe: 'F', age: 85, poids: 120, taille: 150, activite: 'sedentaire', objectif: 'seche' });
ok(r.carbs === 0 && r.kcal === 4 * r.protein + 9 * r.fat && r.notes.length === 1, 'glucides negatifs -> 0, kcal relevees');
r = NS.calcTargets({ sexe: 'F', age: 60, poids: 40, taille: 150, activite: 'sedentaire', objectif: 'seche' });
ok(r.kcal >= r.bmr, 'seche jamais sous le metabolisme de base');
ok(NS.calcTargets({ sexe: 'H', age: 10, poids: 30, taille: 140, activite: 'modere', objectif: 'masse' }).error, 'age < 14 refuse');
ok(NS.calcTargets({ sexe: 'X', age: 30, poids: 80, taille: 180, activite: 'modere', objectif: 'masse' }).error, 'sexe invalide');
ok(NS.calcTargets({ sexe: 'H', age: 30, poids: 'abc', taille: 180, activite: 'modere', objectif: 'masse' }).error, 'poids invalide');
let m = NS.ringModel(120, 165, 'Protéines', 'g');
eq(m.text, '120 g / 165 g Protéines — Reste 45 g'); ok(!m.over && Math.abs(m.frac - 120 / 165) < 1e-9);
m = NS.ringModel(200, 165, 'Protéines', 'g'); ok(m.over && m.text.endsWith('Dépassé de 35 g') && m.frac === 1);
m = NS.ringModel(170, 165, 'Protéines', 'g'); ok(!m.over, 'tolerance 5 %');
eq(NS.ringModel(50, 0, 'Lipides', 'g').frac, 0);
eq(NS.dayKey(new Date(2026, 0, 5)), '2026-01-05'); eq(NS.dayKey(new Date(2026, 10, 14)), '2026-11-14');
const t = NS.sumEntries({ a: { calories: '100', proteines: 10, glucides: 'x', lipides: 2, poids_g: 150 }, b: { calories: 50.5, proteines: 5 } });
eq([t.kcal, t.protein, t.carbs, t.fat, t.weight, t.count], [150.5, 15, 0, 2, 150, 2]);
ok(NS.isOnline(100000, 120000, 0, 45000) && !NS.isOnline(100000, 160000, 0, 45000) && !NS.isOnline(0, 1, 0));
ok(NS.isOnline(100000, 150000, -10000, 45000), 'decalage horloge client/serveur');
eq(NS.ago(0, 5000, 0), 'il y a 5 s'); eq(NS.ago(0, 180000, 0), 'il y a 3 min');

// ---------- 2. identifiants HTML <-> JS ----------
const htmlIds = new Set([...html.matchAll(/\sid="([^"]+)"/g)].map((x) => x[1]));
const used = new Set([...appSrc.matchAll(/\$\('([^']+)'\)/g)].map((x) => x[1]).filter((id) => !id.includes("' +")));
const dynamic = new Set(['ring-kcal', 'ring-protein', 'ring-carbs', 'ring-fat']);
const missing = [...used].filter((id) => !htmlIds.has(id) && !dynamic.has(id));
eq(missing, [], 'ids utilises par le JS mais absents du HTML : ' + missing);
ok(!/<section id="tab-profile"[^>]*\bhidden\b/.test(html), "l'onglet profil ne doit pas avoir l'attribut hidden (bloque l'affichage)");
fs.writeFileSync('/tmp/ns_app.mjs', appSrc);
require('child_process').execFileSync(process.execPath, ['--check', '/tmp/ns_app.mjs']);   // syntaxe du module

// ---------- 3. execution de l'interface avec faux DOM + faux Firebase ----------
const registry = {}; const handlers = {};
function makeEl(id) {
  const e = { id: id || '', value: '', textContent: '', innerHTML: '', hidden: false, disabled: false, className: '', title: '', style: {}, dataset: {}, _qs: {},
    classList: { toggle() {}, add() {}, remove() {} },
    addEventListener(type, fn) { handlers[(e.id || '?') + ':' + type] = fn; },
    append() {}, setAttribute() {}, closest() { return null; },
    appendChild(c) { if (c && c.id) registry[c.id] = c; return c; },
    querySelector(sel) { return e._qs[sel] || (e._qs[sel] = makeEl()); }, querySelectorAll() { return []; } };
  return e;
}
const document = {
  getElementById(id) { return registry[id] || (registry[id] = makeEl(id)); },
  createElement() { return makeEl(); }, createTextNode(t) { return { text: t }; }, querySelectorAll() { return []; }
};
const listeners = {}; const writes = []; const store = { ns_scale_id: 'abcdefghij0123456789ABCD' };
const SID = store.ns_scale_id;
const sb = {
  window: { NS }, document, console, Date, Math, JSON, Object, Number, String, Promise, Array, Set, parseInt, parseFloat, isFinite,
  localStorage: { getItem: (k) => store[k] || null, setItem: (k, v) => { store[k] = v; }, removeItem: (k) => { delete store[k]; } },
  location: { reload() {} }, confirm: () => true, setInterval: () => 0, setTimeout: () => 0, clearTimeout: () => {}, alert() {},
  initializeApp: () => ({}), getDatabase: () => ({}), ref: (db, p) => ({ path: p }), serverTimestamp: () => ({ '.sv': 'timestamp' }),
  onValue: (r, cb) => { (listeners[r.path] = listeners[r.path] || []).push(cb); return () => {}; },
  set: (r, v) => { writes.push(['set', r.path, v]); return Promise.resolve(); },
  update: (r, v) => { writes.push(['update', r.path, v]); return Promise.resolve(); },
  remove: (r) => { writes.push(['remove', r.path]); return Promise.resolve(); }
};
// remplace les imports par les fonctions simulees ci-dessus et exige que le code s'execute sans exception
const code = appSrc.replace(/^import .*$/gm, '').replace('https://VOTRE-PROJET-default-rtdb', 'https://demo-default-rtdb');
vm.runInNewContext(code, sb);
const push = (p, v) => (listeners[p] || []).forEach((cb) => cb({ val: () => v }));
const base = 'scales/' + SID; const day = NS.dayKey(new Date());
ok(listeners[base + '/status'] && listeners[base + '/lastScan'] && listeners[base + '/journal/' + day] && listeners[base + '/targets'], 'abonnements Firebase crees');

push('.info/serverTimeOffset', 0); push('.info/connected', true);
eq(registry.pillCloud.textContent, 'Cloud connecté');
push(base + '/status', { ts: Date.now() - 3000, weight_g: 182.46, phase: 'settling', rssi: -55, cam_ok: true, hx711_ok: true, model: 'qwen3-vl:4b-instruct' });
eq(registry.pillScale.textContent, 'Balance en ligne'); eq(registry.livePhase.textContent, 'Stabilisation…');
eq(registry.btnTare.disabled, false);
push(base + '/status', { ts: Date.now() - 120000, weight_g: 10, phase: 'empty' });
eq(registry.pillScale.textContent, 'Balance hors ligne'); eq(registry.btnTare.disabled, true);

push(base + '/targets', { kcal: 3035, proteines: 165, glucides: 419, lipides: 80 });
push(base + '/journal/' + day, { s1: { aliment: 'Riz', poids_g: 200, calories: 260, proteines: 5, glucides: 56, lipides: 0.6, ts: Date.now() } });
eq(registry['ring-protein'].querySelector('.ring-sub').textContent, '5 g / 165 g Protéines — Reste 160 g');
push(base + '/lastScan', { id: 's2', ts: Date.now() - 4000, aliment: 'Poulet grillé', poids_g: 150, proteines: 46.5, glucides: 0, lipides: 5.4, calories: 235, conseil_coach: 'Excellent apport en protéines.', objectif: 'masse' });
eq(registry.scName.textContent, 'Poulet grillé'); eq(registry.btnAdd.disabled, false);
handlers['btnAdd:click']();
const add = writes.find((w) => w[0] === 'set' && w[1] === base + '/journal/' + day + '/s2'); ok(add && add[2].proteines === 46.5 && add[2].aliment === 'Poulet grillé', 'ajout au journal (cle = id du scan)');
push(base + '/journal/' + day, { s1: {}, s2: { aliment: 'Poulet grillé', poids_g: 150 } });
eq(registry.btnAdd.disabled, true, 'pas de double ajout'); eq(registry.btnAdd.textContent, 'Déjà ajouté ✔');
push(base + '/status', { ts: Date.now(), phase: 'empty', weight_g: 0 });
handlers['btnTare:click'](); ok(writes.some((w) => w[0] === 'set' && w[1] === base + '/command/tare'), 'tare a distance envoyee');
handlers['btnReset:click'](); ok(writes.some((w) => w[0] === 'remove' && w[1] === base + '/journal/' + day), 'reinitialisation du jour');

const E = (id) => document.getElementById(id);
E('fName').value = 'Alex'; E('fW').value = '80'; E('fH').value = '180'; E('fA').value = '30'; E('fS').value = 'H'; E('fAct').value = 'modere'; E('fGoal').value = 'masse';
handlers['profileForm:submit']({ preventDefault() {} });
const up = writes.find((w) => w[0] === 'update' && w[1] === base); ok(up && up[2].goal === 'masse' && up[2].targets.kcal === 3035 && up[2].targets.proteines === 160 && up[2].profile.prenom === 'Alex', 'profil + objectifs enregistres');
E('fW').value = '5'; const before = writes.length; handlers['profileForm:submit']({ preventDefault() {} }); eq(writes.length, before, 'profil invalide : rien d\'ecrit');
handlers['gateForm:submit']({ preventDefault() {} });

console.log(`${n} verifications web OK`);
