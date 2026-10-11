'use strict';

/**
 * Запрос к API сервера; при ошибке бросает Error с текстом из ответа.
 */
async function api(method, path, body) {
    const res = await fetch('/api/' + path, {
        method,
        headers: body ? {'Content-Type': 'application/json'} : {},
        body: body ? JSON.stringify(body) : undefined,
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok) {
        throw new Error(data.error || res.statusText);
    }
    return data;
}

api('GET', 'version')
    .then(v => document.getElementById('version').textContent = `bakery ${v.version}, схема ${v.schema}`)
    .catch(e => document.getElementById('version').textContent = 'сервер недоступен: ' + e.message);
