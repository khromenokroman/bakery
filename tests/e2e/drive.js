// Драйвер Chrome по CDP через --remote-debugging-pipe без зависимостей: node drive.js <url> <scenario.js | --dump>
// Сценарий - тело async-функции в странице с помощниками sleep, btn(текст), expect(условие, текст).
// Код возврата 1, если сценарий бросил исключение; печатается журнал log.
// --dump вместо сценария печатает DOM страницы после загрузки.
// COOKIE=bakery_session=... ставит cookie сессии до открытия страницы (вход без формы).
// E2E_TIMEOUT - предел сценария в секундах (по умолчанию 20).
const {spawn} = require('child_process');
const fs = require('fs');
const [url, scenarioFile] = process.argv.slice(2);
const dump = scenarioFile === '--dump';
const scenario = dump ? '' : fs.readFileSync(scenarioFile, 'utf8');
const chrome = spawn(process.env.CHROME || 'google-chrome', ['--headless=new', '--disable-gpu', '--no-sandbox', '--remote-debugging-pipe', 'about:blank'],
    {stdio: ['ignore', 'ignore', 'ignore', 'pipe', 'pipe']});
const out = chrome.stdio[3], inp = chrome.stdio[4];
let buf = '', nextId = 1; const waiting = new Map();
inp.on('data', d => {
    buf += d.toString();
    let i;
    while ((i = buf.indexOf('\0')) >= 0) {
        const msg = JSON.parse(buf.slice(0, i)); buf = buf.slice(i + 1);
        if (msg.id && waiting.has(msg.id)) { waiting.get(msg.id)(msg); waiting.delete(msg.id); }
    }
});
const send = (method, params = {}, sessionId) => new Promise(res => {
    const id = nextId++; waiting.set(id, res);
    out.write(JSON.stringify({id, method, params, sessionId}) + '\0');
});
(async () => {
    const {result: {targetId}} = await send('Target.createTarget', {url: 'about:blank'});
    const {result: {sessionId}} = await send('Target.attachToTarget', {targetId, flatten: true});
    await send('Runtime.enable', {}, sessionId);
    await send('Page.enable', {}, sessionId);
    if (process.env.COOKIE) {
        const [name, value] = process.env.COOKIE.split(/=(.*)/);
        await send('Network.setCookie', {name, value, url}, sessionId);
    }
    await send('Page.navigate', {url}, sessionId);
    await new Promise(r => setTimeout(r, 1500));
    if (dump) {
        const d = await send('Runtime.evaluate', {expression: 'document.documentElement.outerHTML', returnByValue: true}, sessionId);
        console.log(d.result.result.value);
        chrome.kill();
        process.exit(0);
    }
    const limit = Number(process.env.E2E_TIMEOUT || 20);
    setTimeout(() => { console.log(`    ОШИБКА: сценарий не завершился за ${limit} с`); chrome.kill(); process.exit(1); }, limit * 1000);
    const r = await send('Runtime.evaluate', {
        expression: `(async () => { const sleep = ms => new Promise(r => setTimeout(r, ms));
            const btn = t => [...document.querySelectorAll('button')].find(b => b.textContent.trim() === t);
            const expect = (cond, text) => { if (!cond) throw new Error(text); };
            const log = [];
            window.confirm = () => true;
            try { ${scenario} } catch (e) { return {ok: false, error: e.message, log}; }
            return {ok: true, log}; })()`,
        awaitPromise: true, returnByValue: true}, sessionId);
    const value = r.result && r.result.result ? r.result.result.value : null;
    chrome.kill();
    if (!value) {
        console.log('сценарий не выполнился: ' + JSON.stringify(r));
        process.exit(1);
    }
    value.log.forEach(line => console.log('    ' + line));
    if (!value.ok) {
        console.log('    ОШИБКА: ' + value.error);
    }
    process.exit(value.ok ? 0 : 1);
})();
