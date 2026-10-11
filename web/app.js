'use strict';

const KINDS = {product: 'Продукция', semi: 'Полуфабрикат', raw: 'Сырьё'};
const UNITS = {kg: 'кг', l: 'л', pcs: 'шт'};

/**
 * Запрос к API сервера; при ошибке бросает Error с текстом из ответа.
 */
async function api(method, path, body) {
    const res = await fetch('/api/' + path, {
        method,
        headers: body !== undefined ? {'Content-Type': 'application/json'} : {},
        body: body !== undefined ? JSON.stringify(body) : undefined,
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok) {
        throw new Error(data && data.error ? data.error : res.status + ' ' + res.statusText);
    }
    return data;
}

/**
 * Создаёт элемент: el('td', {class: 'num'}, 'текст', другойЭлемент).
 * Строки вставляются как текст, не как HTML.
 */
function el(tag, attrs, ...children) {
    const node = document.createElement(tag);
    for (const [key, value] of Object.entries(attrs || {})) {
        if (key.startsWith('on')) {
            node.addEventListener(key.slice(2), value);
        } else if (value === true) {
            node.setAttribute(key, '');
        } else if (value !== false && value !== null && value !== undefined) {
            node.setAttribute(key, value);
        }
    }
    for (const child of children.flat()) {
        if (child !== null && child !== undefined && child !== false) {
            node.append(child instanceof Node ? child : String(child));
        }
    }
    return node;
}

function options(map, selected) {
    return Object.entries(map).map(([value, text]) => el('option', {value, selected: value === selected}, text));
}

/**
 * Количество в тысячных долях из строки "1,5" или "1.5" без потерь округления.
 * Возвращает null, если строка не число с не более чем тремя знаками после запятой.
 */
function parseQty(text) {
    const m = String(text).trim().match(/^(\d{1,7})(?:[.,](\d{1,3}))?$/);
    if (!m) {
        return null;
    }
    return Number(m[1]) * 1000 + Number((m[2] || '').padEnd(3, '0'));
}

/** Тысячные доли в строку с запятой: 1500 -> "1,5". */
function formatQty(milli) {
    const whole = Math.trunc(milli / 1000);
    const frac = String(Math.abs(milli % 1000)).padStart(3, '0').replace(/0+$/, '');
    return frac ? `${whole},${frac}` : String(whole);
}

function showMessage(text, kind = 'error') {
    const box = document.getElementById('message');
    box.textContent = text;
    box.className = kind;
    box.hidden = !text;
}

/** Выполняет действие и показывает ошибку вместо исключения. */
async function attempt(action, okText) {
    try {
        await action();
        showMessage(okText || '', 'ok');
        return true;
    } catch (e) {
        showMessage(e.message);
        return false;
    }
}

// ---------- номенклатура: список ----------

const listState = {kind: '', archived: false};

async function renderItems(view) {
    const query = new URLSearchParams();
    if (listState.kind) {
        query.set('kind', listState.kind);
    }
    if (listState.archived) {
        query.set('archived', '1');
    }
    const items = await api('GET', 'items?' + query);

    const tabs = el('div', {class: 'tabs toolbar'},
        Object.entries({'': 'Все', ...KINDS}).map(([kind, text]) =>
            el('button', {
                class: listState.kind === kind ? 'active' : null,
                onclick: () => { listState.kind = kind; route(); },
            }, text)),
        el('label', {class: 'check'},
            el('input', {
                type: 'checkbox', checked: listState.archived,
                onchange: e => { listState.archived = e.target.checked; route(); },
            }),
            'показать архив'));

    const name = el('input', {name: 'name', placeholder: 'Название', required: true, size: 30});
    const kind = el('select', {name: 'kind'}, options(KINDS, listState.kind || 'product'));
    const unit = el('select', {name: 'unit'}, options(UNITS, 'pcs'));
    const form = el('form', {
        class: 'inline',
        onsubmit: async e => {
            e.preventDefault();
            let created;
            if (await attempt(async () => {
                created = await api('POST', 'items', {name: name.value, kind: kind.value, unit: unit.value});
            }, '')) {
                location.hash = '#/items/' + created.id;
            }
        },
    }, name, kind, unit, el('button', {class: 'primary'}, 'Добавить'));

    const rows = items.map(item => el('tr', {
            class: 'link' + (item.archived ? ' archived' : ''),
            onclick: () => { location.hash = '#/items/' + item.id; },
        },
        el('td', {}, item.name, item.archived ? el('span', {class: 'muted'}, ' (архив)') : null),
        el('td', {}, KINDS[item.kind]),
        el('td', {}, UNITS[item.unit]),
        el('td', {}, item.kind === 'raw' ? '' : item.has_recipe ? 'есть' : el('span', {class: 'muted'}, 'нет'))));

    view.replaceChildren(
        el('h2', {}, 'Номенклатура'),
        form,
        el('div', {style: 'height: 16px'}),
        tabs,
        el('div', {class: 'table-wrap'}, el('table', {},
            el('thead', {}, el('tr', {}, el('th', {}, 'Название'), el('th', {}, 'Вид'), el('th', {}, 'Ед.'), el('th', {}, 'Рецептура'))),
            el('tbody', {}, rows.length ? rows : el('tr', {}, el('td', {colspan: 4, class: 'muted'}, 'Пусто'))))));
}

// ---------- номенклатура: карточка ----------

async function renderItem(view, id) {
    const [item, recipe, all] = await Promise.all([
        api('GET', `items/${id}`),
        api('GET', `items/${id}/recipe`),
        api('GET', 'items?archived=1'),
    ]);

    const name = el('input', {value: item.name, size: 30});
    const kind = el('select', {}, options(KINDS, item.kind));
    const unit = el('select', {}, options(UNITS, item.unit));
    const archived = el('input', {type: 'checkbox', checked: item.archived});

    const save = async () => {
        if (await attempt(() => api('PUT', `items/${id}`, {
            name: name.value, kind: kind.value, unit: unit.value, archived: archived.checked,
        }), 'Сохранено')) {
            route();
        }
    };
    const remove = async () => {
        if (!confirm(`Удалить «${item.name}»?`)) {
            return;
        }
        if (await attempt(() => api('DELETE', `items/${id}`), 'Удалено')) {
            location.hash = '#/items';
        }
    };

    view.replaceChildren(
        el('a', {class: 'back', href: '#/items'}, '← Номенклатура'),
        el('h2', {}, item.name),
        el('div', {class: 'fields'},
            el('label', {}, 'Название', name),
            el('label', {}, 'Вид', kind),
            el('label', {}, 'Единица', unit),
            el('label', {class: 'check'}, archived, 'в архиве'),
            el('button', {class: 'primary', onclick: save}, 'Сохранить'),
            el('button', {class: 'danger', onclick: remove}, 'Удалить')),
        item.kind === 'raw'
            ? el('p', {class: 'muted'}, 'У сырья нет рецептуры.')
            : recipeEditor(item, recipe, all));
}

/**
 * Редактор рецептуры: выход и строки компонентов; сохраняется целиком.
 */
function recipeEditor(item, recipe, all) {
    const used = new Set((recipe ? recipe.lines : []).map(l => l.component_id));
    // в выбор попадают все, кроме самой позиции; архивные только если уже есть в рецептуре
    const candidates = all.filter(i => i.id !== item.id && (!i.archived || used.has(i.id)));
    const byId = new Map(all.map(i => [i.id, i]));

    const output = el('input', {class: 'qty', value: recipe ? formatQty(recipe.output_qty) : '1'});
    const tbody = el('tbody');

    const componentSelect = selected => {
        const select = el('select', {}, el('option', {value: ''}, '— выберите —'));
        for (const [k, title] of Object.entries(KINDS)) {
            const group = el('optgroup', {label: title},
                candidates.filter(i => i.kind === k).map(i =>
                    el('option', {value: i.id, selected: i.id === selected}, i.name + (i.archived ? ' (архив)' : ''))));
            if (group.children.length) {
                select.append(group);
            }
        }
        return select;
    };

    const addRow = line => {
        const select = componentSelect(line ? line.component_id : null);
        const unitCell = el('td');
        const updateUnit = () => {
            const c = byId.get(Number(select.value));
            unitCell.textContent = c ? UNITS[c.unit] : '';
        };
        select.addEventListener('change', updateUnit);
        updateUnit();
        const row = el('tr', {},
            el('td', {}, select),
            el('td', {class: 'num'}, el('input', {class: 'qty', value: line ? formatQty(line.qty) : ''})),
            unitCell,
            el('td', {}, el('button', {class: 'danger', title: 'Убрать строку', onclick: () => row.remove()}, '✕')));
        tbody.append(row);
    };
    (recipe ? recipe.lines : []).forEach(addRow);
    if (!recipe) {
        addRow(null);
    }

    const save = async () => {
        const outputQty = parseQty(output.value);
        if (outputQty === null) {
            showMessage('Выход: введите число, не более трёх знаков после запятой');
            return;
        }
        const lines = [];
        for (const row of tbody.children) {
            const componentId = Number(row.querySelector('select').value);
            const qtyText = row.querySelector('input').value;
            if (!componentId && !qtyText.trim()) {
                continue;
            }
            if (!componentId) {
                showMessage('В строке рецептуры не выбран компонент');
                return;
            }
            const qty = parseQty(qtyText);
            if (qty === null) {
                showMessage(`«${byId.get(componentId).name}»: введите количество, не более трёх знаков после запятой`);
                return;
            }
            lines.push({component_id: componentId, qty});
        }
        if (await attempt(() => api('PUT', `items/${item.id}/recipe`, {output_qty: outputQty, lines}), 'Рецептура сохранена')) {
            route();
        }
    };
    const remove = async () => {
        if (confirm(`Удалить рецептуру «${item.name}»?`) && await attempt(() => api('DELETE', `items/${item.id}/recipe`), 'Рецептура удалена')) {
            route();
        }
    };

    return el('section', {},
        el('h3', {}, 'Рецептура'),
        el('div', {class: 'fields'},
            el('label', {}, 'Выход', el('span', {}, output, ' ', UNITS[item.unit]))),
        el('div', {class: 'table-wrap'}, el('table', {},
            el('thead', {}, el('tr', {}, el('th', {}, 'Компонент'), el('th', {class: 'num'}, 'Количество'), el('th', {}, 'Ед.'), el('th'))),
            tbody)),
        el('div', {class: 'toolbar', style: 'margin-top: 8px'},
            el('button', {onclick: () => addRow(null)}, '+ строка'),
            el('button', {class: 'primary', onclick: save}, 'Сохранить рецептуру'),
            recipe ? el('button', {class: 'danger', onclick: remove}, 'Удалить рецептуру') : null));
}

// ---------- маршрутизация ----------

async function route() {
    const view = document.getElementById('view');
    const hash = location.hash || '#/items';
    const section = hash.split('/')[1];
    for (const a of document.querySelectorAll('nav a[data-section]')) {
        a.classList.toggle('active', a.dataset.section === section);
    }
    try {
        let m;
        if ((m = hash.match(/^#\/items\/(\d+)$/))) {
            await renderItem(view, Number(m[1]));
        } else {
            await renderItems(view);
        }
    } catch (e) {
        showMessage(e.message);
    }
}

window.addEventListener('hashchange', () => { showMessage(''); route(); });
route();

api('GET', 'version')
    .then(v => document.getElementById('version').textContent = `bakery ${v.version}, схема ${v.schema}`)
    .catch(e => document.getElementById('version').textContent = 'сервер недоступен: ' + e.message);
