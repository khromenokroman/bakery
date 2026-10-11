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

/**
 * Заменяет содержимое view; null, undefined и false пропускаются, как в el().
 * replaceChildren сам вставил бы их текстом "null".
 */
function render(view, ...children) {
    view.replaceChildren(...children.flat().filter(c => c !== null && c !== undefined && c !== false));
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

/**
 * Копейки из строки "45", "45,5" или "45.50"; null, если не сумма в рублях с не более чем двумя знаками.
 */
function parseMoney(text) {
    const m = String(text).trim().replace(/\s/g, '').match(/^(\d{1,9})(?:[.,](\d{1,2}))?$/);
    if (!m) {
        return null;
    }
    return Number(m[1]) * 100 + Number((m[2] || '').padEnd(2, '0'));
}

/** Копейки в рубли с разделением разрядов: 1234550 -> "12 345,50". */
function formatMoney(kop) {
    const sign = kop < 0 ? '-' : '';
    const abs = Math.abs(kop);
    const rub = Math.trunc(abs / 100).toString().replace(/\B(?=(\d{3})+(?!\d))/g, '\u00a0');
    return `${sign}${rub},${String(abs % 100).padStart(2, '0')}`;
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

// ---------- группы ----------

/** Группы в порядке дерева: [{group, depth}], соседи по названию. */
function groupTree(groups) {
    const children = new Map();
    for (const g of groups) {
        const key = g.parent_id ?? 0;
        children.set(key, [...(children.get(key) || []), g]);
    }
    const out = [];
    const walk = (parent, depth) => {
        for (const g of (children.get(parent) || []).sort((a, b) => a.name.localeCompare(b.name, 'ru'))) {
            out.push({group: g, depth});
            walk(g.id, depth + 1);
        }
    };
    walk(0, 0);
    return out;
}

/** Путь группы: "Кондитерка / Торты". */
function groupPath(groups, id) {
    const byId = new Map(groups.map(g => [g.id, g]));
    const names = [];
    for (let g = byId.get(id); g && names.length < 100; g = byId.get(g.parent_id)) {
        names.unshift(g.name);
    }
    return names.join(' / ');
}

/** Группа и все её подгруппы. */
function groupWithDescendants(groups, id) {
    const out = new Set([id]);
    let grown = true;
    while (grown) {
        grown = false;
        for (const g of groups) {
            if (g.parent_id !== null && out.has(g.parent_id) && !out.has(g.id)) {
                out.add(g.id);
                grown = true;
            }
        }
    }
    return out;
}

/** <option> для выбора группы с отступами по вложенности; exclude - id, которых не показывать. */
function groupOptions(groups, selected, emptyText, exclude = new Set()) {
    return [
        el('option', {value: '', selected: selected === null || selected === undefined}, emptyText),
        ...groupTree(groups).filter(({group}) => !exclude.has(group.id)).map(({group, depth}) =>
            el('option', {value: group.id, selected: group.id === selected}, '   '.repeat(depth) + group.name)),
    ];
}

const selectValue = select => (select.value ? Number(select.value) : null);

// ---------- номенклатура: список ----------

const listState = {kind: '', archived: false, group: null};

/**
 * Дерево групп слева от списка: выбор группы, создание подгруппы, переименование и перенос.
 */
function groupsPanel(groups) {
    const selected = listState.group;
    const node = (id, text, depth) => el('div', {
        class: 'tree-node' + (selected === id ? ' active' : ''),
        style: `padding-left: ${8 + depth * 16}px`,
        onclick: () => { listState.group = id; route(); },
    }, text);

    const editor = el('div', {class: 'panel', hidden: true});
    const openEditor = () => {
        const g = groups.find(x => x.id === selected);
        const name = el('input', {value: g.name, size: 20});
        const parent = el('select', {}, groupOptions(groups, g.parent_id, '— верхний уровень —', groupWithDescendants(groups, g.id)));
        editor.replaceChildren(
            el('label', {}, 'Название', name),
            el('label', {}, 'Входит в', parent),
            el('div', {class: 'toolbar', style: 'margin-top: 8px'},
                el('button', {class: 'primary', onclick: async () => {
                    if (await attempt(() => api('PUT', `groups/${g.id}`, {name: name.value, parent_id: selectValue(parent)}), 'Группа сохранена')) {
                        route();
                    }
                }}, 'Сохранить'),
                el('button', {class: 'danger', onclick: async () => {
                    if (confirm(`Удалить группу «${g.name}»?`) && await attempt(() => api('DELETE', `groups/${g.id}`), 'Группа удалена')) {
                        listState.group = null;
                        route();
                    }
                }}, 'Удалить'),
                el('button', {onclick: () => { editor.hidden = true; }}, 'Отмена')));
        editor.hidden = false;
    };
    const addGroup = async () => {
        const where = selected ? `в «${groupPath(groups, selected)}»` : 'на верхнем уровне';
        const name = prompt(`Новая группа ${where}:`);
        if (!name) {
            return;
        }
        let created;
        if (await attempt(async () => { created = await api('POST', 'groups', {name, parent_id: selected}); }, 'Группа создана')) {
            listState.group = created.id;
            route();
        }
    };

    return el('aside', {class: 'groups'},
        el('div', {class: 'tree'},
            node(null, 'Все группы', 0),
            groupTree(groups).map(({group, depth}) => node(group.id, group.name, depth + 1))),
        el('div', {class: 'toolbar', style: 'margin-top: 8px'},
            el('button', {onclick: addGroup}, '+ группа'),
            selected ? el('button', {onclick: openEditor}, 'Изменить') : null),
        editor);
}

async function renderItems(view) {
    const query = new URLSearchParams();
    if (listState.kind) {
        query.set('kind', listState.kind);
    }
    if (listState.archived) {
        query.set('archived', '1');
    }
    const groups = await api('GET', 'groups');
    if (listState.group && !groups.some(g => g.id === listState.group)) {
        listState.group = null;
    }
    if (listState.group) {
        query.set('group', listState.group);
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

    const name = el('input', {name: 'name', placeholder: 'Название', required: true, size: 24});
    const kind = el('select', {name: 'kind'}, options(KINDS, listState.kind || 'product'));
    const unit = el('select', {name: 'unit'}, options(UNITS, 'pcs'));
    const group = el('select', {name: 'group'}, groupOptions(groups, listState.group, '— без группы —'));
    const form = el('form', {
        class: 'inline',
        onsubmit: async e => {
            e.preventDefault();
            let created;
            if (await attempt(async () => {
                created = await api('POST', 'items', {name: name.value, kind: kind.value, unit: unit.value, group_id: selectValue(group)});
            }, '')) {
                location.hash = '#/items/' + created.id;
            }
        },
    }, name, kind, unit, group, el('button', {class: 'primary'}, 'Добавить'));

    const rows = items.map(item => el('tr', {
            class: 'link' + (item.archived ? ' archived' : ''),
            onclick: () => { location.hash = '#/items/' + item.id; },
        },
        el('td', {}, item.name, item.archived ? el('span', {class: 'muted'}, ' (архив)') : null),
        el('td', {}, KINDS[item.kind]),
        el('td', {}, UNITS[item.unit]),
        el('td', {class: 'muted'}, item.group_id ? groupPath(groups, item.group_id) : ''),
        el('td', {}, item.kind === 'raw' ? '' : item.has_recipe ? 'есть' : el('span', {class: 'muted'}, 'нет'))));

    render(view,
        el('h2', {}, 'Номенклатура'),
        form,
        el('div', {class: 'split'},
            groupsPanel(groups),
            el('div', {class: 'split-main'},
                el('h3', {style: 'margin-top: 0'}, listState.group ? groupPath(groups, listState.group) : 'Все группы'),
                tabs,
                el('div', {class: 'table-wrap'}, el('table', {},
                    el('thead', {}, el('tr', {}, el('th', {}, 'Название'), el('th', {}, 'Вид'), el('th', {}, 'Ед.'), el('th', {}, 'Группа'), el('th', {}, 'Рецептура'))),
                    el('tbody', {}, rows.length ? rows : el('tr', {}, el('td', {colspan: 5, class: 'muted'}, 'Пусто'))))))));
}

// ---------- номенклатура: карточка ----------

async function renderItem(view, id) {
    const [item, recipe, all, groups] = await Promise.all([
        api('GET', `items/${id}`),
        api('GET', `items/${id}/recipe`),
        api('GET', 'items?archived=1'),
        api('GET', 'groups'),
    ]);

    const name = el('input', {value: item.name, size: 30});
    const kind = el('select', {}, options(KINDS, item.kind));
    const unit = el('select', {}, options(UNITS, item.unit));
    const archived = el('input', {type: 'checkbox', checked: item.archived});
    const group = el('select', {}, groupOptions(groups, item.group_id, '— без группы —'));

    const save = async () => {
        if (await attempt(() => api('PUT', `items/${id}`, {
            name: name.value, kind: kind.value, unit: unit.value, archived: archived.checked, group_id: selectValue(group),
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

    render(view,
        el('a', {class: 'back', href: '#/items'}, '← Номенклатура'),
        el('h2', {}, item.name),
        el('div', {class: 'fields'},
            el('label', {}, 'Название', name),
            el('label', {}, 'Вид', kind),
            el('label', {}, 'Единица', unit),
            el('label', {}, 'Группа', group),
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
        el('div', {class: 'toolbar section-head'},
            el('h3', {}, 'Рецептура'),
            el('button', {class: 'primary', onclick: save}, 'Сохранить рецептуру'),
            recipe ? el('button', {class: 'danger', onclick: remove}, 'Удалить рецептуру') : null),
        el('div', {class: 'fields'},
            el('label', {}, 'Выход', el('span', {}, output, ' ', UNITS[item.unit]))),
        el('div', {class: 'table-wrap'}, el('table', {},
            el('thead', {}, el('tr', {}, el('th', {}, 'Компонент'), el('th', {class: 'num'}, 'Количество'), el('th', {}, 'Ед.'), el('th'))),
            tbody)),
        el('div', {class: 'toolbar', style: 'margin-top: 8px'},
            el('button', {onclick: () => addRow(null)}, '+ строка')));
}

// ---------- даты ----------

/** Дата в формате YYYY-MM-DD по местному времени. */
function isoDate(d) {
    const pad = n => String(n).padStart(2, '0');
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
}

function shiftDate(iso, days) {
    const [y, m, d] = iso.split('-').map(Number);
    return isoDate(new Date(y, m - 1, d + days));
}

/** "2026-10-12" -> "12.10.2026, пн". */
function humanDate(iso) {
    const [y, m, d] = iso.split('-').map(Number);
    const wd = ['вс', 'пн', 'вт', 'ср', 'чт', 'пт', 'сб'][new Date(y, m - 1, d).getDay()];
    return `${String(d).padStart(2, '0')}.${String(m).padStart(2, '0')}.${y}, ${wd}`;
}

/** «От кого» заявки: клиент из справочника, иначе комментарий. */
function orderFrom(o) {
    return [o.partner_name, o.comment].filter(Boolean).join(', ');
}

function lineSummary(lines) {
    return lines.map(l => `${l.name} ${formatQty(l.qty)} ${UNITS[l.unit]}`).join(', ');
}

// ---------- заявки ----------

async function renderOrders(view, params) {
    const date = params.get('date') || isoDate(new Date());
    const orders = await api('GET', `docs?type=order&from=${date}&to=${date}`);
    const go = d => { location.hash = '#/orders?date=' + d; };

    const picker = el('input', {type: 'date', value: date, onchange: e => e.target.value && go(e.target.value)});
    const checks = new Map();
    const rows = orders.map(doc => {
        const check = el('input', {type: 'checkbox', checked: true, onclick: e => e.stopPropagation()});
        checks.set(doc.id, check);
        return el('tr', {class: 'link', onclick: () => { location.hash = '#/orders/' + doc.id; }},
            el('td', {}, check),
            el('td', {class: 'num'}, doc.number),
            el('td', {}, orderFrom(doc) || el('span', {class: 'muted'}, 'не указано')),
            el('td', {}, lineSummary(doc.lines)));
    });

    const calculate = () => {
        const ids = [...checks].filter(([, c]) => c.checked).map(([id]) => id);
        if (!ids.length) {
            showMessage('Отметьте хотя бы одну заявку');
            return;
        }
        location.hash = `#/requirements?date=${date}&docs=${ids.join(',')}`;
    };

    render(view,
        el('h2', {}, 'Заявки'),
        el('div', {class: 'toolbar'},
            el('button', {title: 'Предыдущий день', onclick: () => go(shiftDate(date, -1))}, '←'),
            picker,
            el('button', {title: 'Следующий день', onclick: () => go(shiftDate(date, 1))}, '→'),
            el('span', {class: 'muted'}, humanDate(date)),
            el('button', {class: 'primary', onclick: () => { location.hash = '#/orders/new?date=' + date; }}, 'Новая заявка')),
        el('div', {class: 'table-wrap'}, el('table', {},
            el('thead', {}, el('tr', {}, el('th'), el('th', {class: 'num'}, '№'), el('th', {}, 'От кого'), el('th', {}, 'Состав'))),
            el('tbody', {}, rows.length ? rows : el('tr', {}, el('td', {colspan: 4, class: 'muted'}, 'На эту дату заявок нет'))))),
        rows.length ? el('div', {class: 'toolbar', style: 'margin-top: 12px'},
            el('button', {class: 'primary', onclick: calculate}, 'Рассчитать потребность')) : null);
}

/** Настройки карточки по виду документа. */
const DOC_TYPES = {
    order: {
        title: 'Заявка', create: 'Новая заявка', comment: 'Комментарий', partner: 'От кого',
        back: d => '#/orders?date=' + d, backTitle: '← Заявки', posting: false, fill: false,
        columns: {qty: true},
    },
    production: {
        title: 'Поступление из производства', create: 'Новое поступление', comment: 'Комментарий',
        back: () => '#/docs', backTitle: '← Документы', posting: true, fill: true,
        columns: {qty: true, cost: true},
    },
    sale: {
        title: 'Реализация', create: 'Новая реализация', comment: 'Комментарий', partner: 'Клиент',
        back: () => '#/docs', backTitle: '← Документы', posting: true, fill: true, priceType: true, typedPrices: true,
        columns: {qty: true, price: true, cost: true},
    },
    prices: {
        title: 'Установка цен', create: 'Новая установка цен', comment: 'Комментарий',
        back: () => '#/prices', backTitle: '← Цены', posting: true, fill: false, priceType: true,
        columns: {price: true},
    },
};

/** Сумма строки в копейках: количество (тысячные) × цена за единицу, с округлением; BigInt против потери точности. */
function lineSum(qty, priceKop) {
    return Number((BigInt(qty) * BigInt(priceKop) + 500n) / 1000n);
}

/**
 * Карточка документа; id = null для нового (вид берётся из params.type, по умолчанию заявка).
 * Колонки зависят от вида: количество, цена (в реализации - из типа цен, можно поправить),
 * сумма, себестоимость на дату документа и её сумма.
 */
async function renderDoc(view, id, params) {
    const [doc, products, priceTypes, partners] = await Promise.all([
        id ? api('GET', `docs/${id}`) : Promise.resolve(null),
        api('GET', 'items?kind=product&archived=1'),
        api('GET', 'price-types'),
        api('GET', 'partners?archived=1'),
    ]);
    const type = doc ? doc.type : (params.get('type') || 'order');
    const cfg = DOC_TYPES[type];
    if (!cfg) {
        throw new Error('неизвестный вид документа: ' + type);
    }
    const cols = cfg.columns;
    const posted = doc && doc.status === 'posted';
    const inDoc = new Set((doc ? doc.lines : []).map(l => l.item_id));
    const candidates = products.filter(p => !p.archived || inDoc.has(p.id));
    const byId = new Map(products.map(p => [p.id, p]));
    const date = doc ? doc.date : (params.get('date') || isoDate(new Date()));

    const dateInput = el('input', {type: 'date', value: date});
    const comment = el('input', {value: doc ? doc.comment : '', size: 40});
    const chosenType = doc ? doc.price_type_id : (params.get('price_type') ? Number(params.get('price_type')) : null);
    const priceType = el('select', {},
        el('option', {value: '', selected: !chosenType}, '— выберите —'),
        priceTypes.map(t => el('option', {value: t.id, selected: t.id === chosenType}, t.name)));
    const partnerId = doc ? doc.partner_id : null;
    const partner = el('select', {},
        el('option', {value: '', selected: !partnerId}, '— не выбран —'),
        partners.filter(p => !p.archived || p.id === partnerId).map(p =>
            el('option', {value: p.id, selected: p.id === partnerId}, p.name + (p.archived ? ' (архив)' : ''))));
    const tbody = el('tbody');
    const totals = el('div', {class: 'totals'});

    // себестоимость и типовые цены на дату документа; у сохранённых строк себестоимость своя (записана сервером)
    let costs = new Map();
    let typed = new Map();
    const loadPrices = async () => {
        const d = dateInput.value;
        if (cols.cost && d) {
            costs = new Map((await api('GET', `prices?type=1&date=${d}`)).map(r => [r.item_id, r.price_kop]));
        }
        if (cfg.typedPrices && d && priceType.value) {
            typed = new Map((await api('GET', `prices?type=${priceType.value}&date=${d}`)).map(r => [r.item_id, r.price_kop]));
        } else {
            typed = new Map();
        }
    };
    await loadPrices();

    const numberOrNull = text => (text === '' || text === undefined ? null : Number(text));

    /** Пересчёт сумм строк, отметок цены и итогов. */
    const update = () => {
        let sum = 0;
        let costSum = 0;
        let missingCost = false;
        let missingPrice = 0;
        for (const row of tbody.children) {
            const itemId = Number(row.querySelector('select').value);
            const qty = cols.qty ? parseQty(row.querySelector('input.qty').value) : null;
            if (cols.price) {
                const input = row.querySelector('input.money');
                const price = parseMoney(input.value);
                const typical = typed.get(itemId);
                input.classList.toggle('missing', !!itemId && cfg.typedPrices && price === null);
                input.classList.toggle('manual', cfg.typedPrices && price !== null && typical !== undefined && price !== typical);
                input.title = !cfg.typedPrices ? '' : typical === undefined ? 'нет цены в выбранном типе цен' : 'типовая цена ' + formatMoney(typical);
                if (itemId && cfg.typedPrices && price === null) {
                    missingPrice++;
                }
                const lineTotal = qty !== null && price !== null && cols.qty ? lineSum(qty, price) : null;
                const cell = row.querySelector('td.sum');
                if (cell) {
                    cell.textContent = lineTotal === null ? '' : formatMoney(lineTotal);
                }
                sum += lineTotal || 0;
            }
            if (cols.cost) {
                const cost = numberOrNull(row.dataset.cost);
                if (itemId && cost === null) {
                    missingCost = true;
                }
                const lineCost = qty !== null && cost !== null ? lineSum(qty, cost) : null;
                row.querySelector('td.cost-sum').textContent = lineCost === null ? '' : formatMoney(lineCost);
                costSum += lineCost || 0;
            }
        }
        const parts = [];
        if (cols.price && cols.qty) {
            parts.push(el('span', {}, 'Сумма: ', el('strong', {}, formatMoney(sum) + ' ₽')));
        }
        if (cols.cost) {
            parts.push(el('span', {title: missingCost ? 'у части строк нет себестоимости' : ''},
                'Себестоимость: ', el('strong', {}, formatMoney(costSum) + ' ₽' + (missingCost ? ' *' : ''))));
        }
        if (cols.price && cols.qty && cols.cost && costSum > 0 && !missingCost) {
            const margin = sum - costSum;
            parts.push(el('span', {}, 'Наценка: ', el('strong', {}, `${formatMoney(margin)} ₽ (${Math.round(margin * 100 / costSum)}%)`)));
        } else if (cols.price && cols.qty && cols.cost && missingCost) {
            parts.push(el('span', {class: 'muted'}, 'наценка не считается: у части строк нет себестоимости'));
        }
        if (missingPrice) {
            parts.push(el('span', {class: 'error-text'}, `без цены строк: ${missingPrice}, провести нельзя`));
        }
        totals.replaceChildren(...parts);
    };

    const addRow = line => {
        const select = el('select', {}, el('option', {value: ''}, '— выберите —'),
            candidates.map(p => el('option', {value: p.id, selected: line && p.id === line.item_id}, p.name + (p.archived ? ' (архив)' : ''))));
        const unitCell = el('td');
        const costCell = el('td', {class: 'num'});
        const priceInput = el('input', {class: 'money', value: line && line.price_kop !== null && line.price_kop !== undefined ? formatMoney(line.price_kop) : '', oninput: update});
        const row = el('tr');
        const setCost = cost => {
            row.dataset.cost = cost === null || cost === undefined ? '' : cost;
            costCell.replaceChildren(cost === null || cost === undefined ? el('span', {class: 'muted', title: 'себестоимость не установлена'}, 'нет') : formatMoney(cost));
        };
        const onItem = initial => {
            const p = byId.get(Number(select.value));
            unitCell.textContent = p ? (cols.price && !cols.qty ? 'за ' : '') + UNITS[p.unit] : '';
            if (cols.cost) {
                setCost(initial && line && line.cost_kop !== undefined ? line.cost_kop : (p ? costs.get(p.id) : null));
            }
            if (cfg.typedPrices && !initial) {
                const price = p ? typed.get(p.id) : undefined;
                priceInput.value = price === undefined ? '' : formatMoney(price);
            }
            update();
        };
        select.addEventListener('change', () => onItem(false));
        row.append(
            el('td', {}, select),
            cols.qty ? el('td', {class: 'num'}, el('input', {class: 'qty', value: line && line.qty ? formatQty(line.qty) : '', oninput: update})) : '',
            unitCell,
            cols.price ? el('td', {class: 'num'}, priceInput) : '',
            cols.price && cols.qty ? el('td', {class: 'num sum'}) : '',
            cols.cost ? costCell : '',
            cols.cost ? el('td', {class: 'num cost-sum'}) : '',
            el('td', {}, el('button', {class: 'danger', title: 'Убрать строку', onclick: () => { row.remove(); update(); }}, '✕')));
        tbody.append(row);
        onItem(!!line && line.price_kop !== undefined);
        return row;
    };
    (doc ? doc.lines : []).forEach(addRow);
    if (!doc) {
        addRow(null);
    }

    /** После смены даты или типа цен: новая себестоимость, по желанию - цены нового типа во всех строках. */
    const reprice = async () => {
        await loadPrices();
        const refill = cfg.typedPrices && tbody.querySelector('select') && confirm('Подставить цены на эту дату и этого типа во все строки? Поправленные вручную цены будут заменены.');
        for (const row of tbody.children) {
            const itemId = Number(row.querySelector('select').value);
            if (cols.cost) {
                const cost = costs.get(itemId);
                row.dataset.cost = cost === undefined ? '' : cost;
                row.querySelector('td.cost-sum').previousSibling.replaceChildren(cost === undefined
                    ? el('span', {class: 'muted', title: 'себестоимость не установлена'}, 'нет') : formatMoney(cost));
            }
            if (refill && itemId) {
                const price = typed.get(itemId);
                row.querySelector('input.money').value = price === undefined ? '' : formatMoney(price);
            }
        }
        update();
    };
    dateInput.addEventListener('change', () => dateInput.value && reprice().catch(err => showMessage(err.message)));
    priceType.addEventListener('change', () => reprice().catch(err => showMessage(err.message)));

    /** Строки из таблицы; null и сообщение, если что-то не так. */
    const collect = () => {
        const lines = [];
        for (const row of tbody.children) {
            const itemId = Number(row.querySelector('select').value);
            const qtyText = cols.qty ? row.querySelector('input.qty').value : '';
            const priceText = cols.price ? row.querySelector('input.money').value : '';
            if (!itemId && !qtyText.trim() && !priceText.trim()) {
                continue;
            }
            if (!itemId) {
                showMessage('В строке не выбрана продукция');
                return null;
            }
            const line = {item_id: itemId};
            if (cols.qty) {
                line.qty = parseQty(qtyText);
                if (line.qty === null) {
                    showMessage(`«${byId.get(itemId).name}»: введите количество, не более трёх знаков после запятой`);
                    return null;
                }
            }
            if (cols.price) {
                line.price_kop = parseMoney(priceText);
                if (line.price_kop === null && (!cfg.typedPrices || priceText.trim())) {
                    showMessage(`«${byId.get(itemId).name}»: введите цену в рублях, не более двух знаков после запятой`);
                    return null;
                }
            }
            lines.push(line);
        }
        return lines;
    };

    const save = async andPost => {
        const lines = collect();
        if (!lines) {
            return;
        }
        const body = {type, date: dateInput.value, comment: comment.value, lines};
        if (cfg.priceType) {
            body.price_type_id = selectValue(priceType);
        }
        if (cfg.partner) {
            body.partner_id = selectValue(partner);
        }
        let saved;
        const ok = await attempt(async () => {
            saved = await api(doc ? 'PUT' : 'POST', doc ? `docs/${doc.id}` : 'docs', body);
            if (andPost) {
                saved = await api('POST', `docs/${saved.id}/post`);
            }
        }, andPost ? 'Сохранено и проведено' : posted ? 'Сохранено и перепроведено' : 'Сохранено');
        if (saved && !doc && cfg.posting) {
            // документ создан: дальше работаем с ним, даже если проведение не удалось
            history.replaceState(null, '', '#/docs/' + saved.id);
        }
        if (ok && !cfg.posting) {
            location.hash = cfg.back(saved.date);
        } else if (saved) {
            const message = document.getElementById('message').textContent;
            const kind = document.getElementById('message').className;
            await renderDoc(view, saved.id, params);
            showMessage(message, kind);
        }
    };
    const unpost = async () => {
        if (await attempt(() => api('POST', `docs/${doc.id}/unpost`), 'Проведение отменено')) {
            await renderDoc(view, doc.id, params);
        }
    };
    const remove = async () => {
        const name = `${cfg.title.toLowerCase()} № ${doc.number}`;
        if (confirm(`Удалить ${name}?`) && await attempt(() => api('DELETE', `docs/${doc.id}`), 'Удалено')) {
            location.hash = cfg.back(doc.date);
        }
    };

    /** Панель «Заполнить по заявкам»: заявки на дату с галочками, строки заменяются суммой. */
    const fillPanel = el('div', {class: 'panel', hidden: true});
    const openFill = async (fillDate) => {
        const orders = await api('GET', `docs?type=order&from=${fillDate}&to=${fillDate}`);
        const checks = orders.map(o => [o, el('input', {type: 'checkbox', checked: true})]);
        const datePick = el('input', {type: 'date', value: fillDate, onchange: e => e.target.value && openFill(e.target.value).catch(err => showMessage(err.message))});
        const apply = async () => {
            const chosen = checks.filter(([, c]) => c.checked).map(([o]) => o);
            if (!chosen.length) {
                showMessage('Отметьте хотя бы одну заявку');
                return;
            }
            const filled = [...tbody.children].some(r => r.querySelector('select').value);
            if (filled && !confirm('Заменить строки документа суммой выбранных заявок?')) {
                return;
            }
            const req = await api('GET', 'requirements?docs=' + chosen.map(o => o.id).join(','));
            tbody.replaceChildren();
            req.products.forEach(p => {
                const row = addRow({item_id: p.item_id, qty: p.qty});
                row.querySelector('select').dispatchEvent(new Event('change'));
            });
            const clients = new Set(chosen.map(o => o.partner_id));
            if (cfg.partner && !partner.value && clients.size === 1 && chosen[0].partner_id) {
                partner.value = chosen[0].partner_id;
            }
            if (!comment.value.trim()) {
                comment.value = `по заявкам № ${chosen.map(o => o.number).join(', ')} на ${humanDate(fillDate).split(',')[0]}`;
            }
            fillPanel.hidden = true;
            update();
            showMessage('Строки заполнены по заявкам, проверьте количества', 'ok');
        };
        fillPanel.replaceChildren(
            el('div', {class: 'toolbar'}, el('strong', {}, 'Заявки на'), datePick,
                el('button', {onclick: () => { fillPanel.hidden = true; }}, 'Закрыть')),
            orders.length
                ? el('div', {}, checks.map(([o, c]) => el('label', {class: 'check'}, c,
                    `№ ${o.number}${orderFrom(o) ? ' (' + orderFrom(o) + ')' : ''}: ${lineSummary(o.lines)}`)))
                : el('p', {class: 'muted'}, 'На эту дату заявок нет'),
            orders.length ? el('div', {class: 'toolbar', style: 'margin-top: 8px'},
                el('button', {class: 'primary', onclick: () => apply().catch(err => showMessage(err.message))}, 'Заполнить')) : null);
        fillPanel.hidden = false;
    };

    const title = doc ? `${cfg.title} № ${doc.number}` : cfg.create;
    render(view,
        el('a', {class: 'back', href: cfg.back(date)}, cfg.backTitle),
        // заголовок и кнопки прилипают к верху окна, чтобы не листать длинный документ
        el('div', {class: 'actions'},
            el('h2', {}, title, cfg.posting && doc ? el('span', {class: 'status ' + doc.status}, posted ? 'проведён' : 'не проведён') : null),
            el('div', {class: 'toolbar'},
                posted
                    ? el('button', {class: 'primary', onclick: () => save(false)}, 'Сохранить и перепровести')
                    : el('button', {class: cfg.posting ? null : 'primary', onclick: () => save(false)}, 'Сохранить'),
                cfg.posting && !posted ? el('button', {class: 'primary', onclick: () => save(true)}, 'Сохранить и провести') : null,
                posted ? el('button', {onclick: unpost}, 'Отменить проведение') : null,
                cfg.fill ? el('button', {onclick: () => openFill(dateInput.value || date).catch(err => showMessage(err.message))}, 'Заполнить по заявкам') : null,
                doc && !posted ? el('button', {class: 'danger', onclick: remove}, 'Удалить') : null)),
        el('div', {class: 'fields'},
            el('label', {}, 'Дата', dateInput),
            cfg.partner ? el('label', {}, cfg.partner, partner) : null,
            cfg.priceType ? el('label', {}, 'Тип цен', priceType) : null,
            el('label', {}, cfg.comment, comment)),
        fillPanel,
        el('div', {class: 'table-wrap', style: 'margin-top: 12px'}, el('table', {},
            el('thead', {}, el('tr', {},
                el('th', {}, 'Продукция'),
                cols.qty ? el('th', {class: 'num'}, 'Количество') : null,
                el('th', {}, 'Ед.'),
                cols.price ? el('th', {class: 'num'}, 'Цена, ₽') : null,
                cols.price && cols.qty ? el('th', {class: 'num'}, 'Сумма, ₽') : null,
                cols.cost ? el('th', {class: 'num'}, 'Себест., ₽') : null,
                cols.cost ? el('th', {class: 'num'}, 'Сумма себест., ₽') : null,
                el('th'))),
            tbody)),
        el('div', {class: 'toolbar', style: 'margin-top: 8px'},
            el('button', {onclick: () => addRow(null).querySelector('select').focus()}, '+ строка')),
        totals);
    update();
}

// ---------- документы ----------

const docsState = {from: null, to: null};

async function renderDocs(view) {
    const today = isoDate(new Date());
    const from = docsState.from || shiftDate(today, -7);
    const to = docsState.to || today;
    const docs = await api('GET', `docs?type=production,sale,prices&from=${from}&to=${to}`);
    const fromInput = el('input', {type: 'date', value: from, onchange: e => { docsState.from = e.target.value; route(); }});
    const toInput = el('input', {type: 'date', value: to, onchange: e => { docsState.to = e.target.value; route(); }});

    const rows = docs.slice().reverse().map(doc => el('tr', {class: 'link', onclick: () => { location.hash = '#/docs/' + doc.id; }},
        el('td', {}, humanDate(doc.date)),
        el('td', {}, `${DOC_TYPES[doc.type].title} № ${doc.number}`),
        el('td', {}, el('span', {class: 'status ' + doc.status}, doc.status === 'posted' ? 'проведён' : 'не проведён')),
        el('td', {}, orderFrom(doc)),
        el('td', {}, doc.type === 'prices' ? doc.lines.map(l => l.name).join(', ') : lineSummary(doc.lines))));

    render(view,
        el('h2', {}, 'Документы'),
        el('div', {class: 'toolbar'},
            el('button', {class: 'primary', onclick: () => { location.hash = '#/docs/new?type=production'; }}, 'Новое поступление'),
            el('button', {class: 'primary', onclick: () => { location.hash = '#/docs/new?type=sale'; }}, 'Новая реализация'),
            el('button', {onclick: () => { location.hash = '#/docs/new?type=prices'; }}, 'Новая установка цен')),
        el('div', {class: 'toolbar'}, 'с', fromInput, 'по', toInput),
        el('div', {class: 'table-wrap'}, el('table', {},
            el('thead', {}, el('tr', {}, el('th', {}, 'Дата'), el('th', {}, 'Документ'), el('th', {}, 'Статус'), el('th', {}, 'Клиент, комментарий'), el('th', {}, 'Состав'))),
            el('tbody', {}, rows.length ? rows : el('tr', {}, el('td', {colspan: 5, class: 'muted'}, 'За эти дни документов нет'))))));
}

// ---------- цены ----------

async function renderPrices(view, params) {
    const types = await api('GET', 'price-types');
    const typeId = Number(params.get('type')) || types[0].id;
    const current = types.find(t => t.id === typeId) || types[0];
    const date = params.get('date') || isoDate(new Date());
    const list = await api('GET', `prices?type=${current.id}&date=${date}`);
    const go = (t, d) => { location.hash = `#/prices?type=${t}&date=${d}`; };

    const addType = async () => {
        const name = prompt('Новый тип цен:');
        let created;
        if (name && await attempt(async () => { created = await api('POST', 'price-types', {name}); }, 'Тип цен создан')) {
            go(created.id, date);
        }
    };
    const renameType = async () => {
        const name = prompt('Название типа цен:', current.name);
        if (name && await attempt(() => api('PUT', `price-types/${current.id}`, {name}), 'Тип цен переименован')) {
            route();
        }
    };
    const removeType = async () => {
        if (confirm(`Удалить тип цен «${current.name}»?`) && await attempt(() => api('DELETE', `price-types/${current.id}`), 'Тип цен удалён')) {
            go(types[0].id, date);
        }
    };

    render(view,
        el('h2', {}, 'Цены'),
        el('div', {class: 'tabs toolbar'},
            types.map(t => el('button', {class: t.id === current.id ? 'active' : null, onclick: () => go(t.id, date)}, t.name)),
            el('button', {onclick: addType}, '+ тип цен')),
        el('div', {class: 'toolbar'},
            el('button', {class: 'primary', onclick: () => { location.hash = `#/docs/new?type=prices&price_type=${current.id}&date=${date}`; }}, 'Новая установка цен'),
            current.system ? null : el('button', {onclick: renameType}, 'Переименовать'),
            current.system ? null : el('button', {class: 'danger', onclick: removeType}, 'Удалить тип')),
        el('div', {class: 'toolbar'},
            'Цены «' + current.name + '» на',
            el('input', {type: 'date', value: date, onchange: e => e.target.value && go(current.id, e.target.value)}),
            el('span', {class: 'muted'}, humanDate(date))),
        current.system ? el('p', {class: 'muted'}, 'Себестоимость подставляется в строки складских документов на их дату.') : null,
        el('div', {class: 'table-wrap'}, el('table', {class: 'needs'},
            el('thead', {}, el('tr', {}, el('th', {}, 'Продукция'), el('th', {class: 'num'}, 'Цена, ₽'), el('th', {}, 'Ед.'), el('th', {}, 'Действует с'))),
            el('tbody', {}, list.length
                ? list.map(r => el('tr', {class: 'link', onclick: () => { location.hash = '#/docs/' + r.doc_id; }},
                    el('td', {}, r.name),
                    el('td', {class: 'num'}, formatMoney(r.price_kop)),
                    el('td', {}, 'за ' + UNITS[r.unit]),
                    el('td', {class: 'muted'}, humanDate(r.date))))
                : el('tr', {}, el('td', {colspan: 4, class: 'muted'}, 'Цен этого типа на дату нет'))))));
}

// ---------- клиенты ----------

const partnersState = {archived: false};

async function renderPartners(view) {
    const partners = await api('GET', 'partners' + (partnersState.archived ? '?archived=1' : ''));
    const name = el('input', {placeholder: 'Название или имя', required: true, size: 28});
    const phone = el('input', {placeholder: 'Телефон', size: 16});
    const note = el('input', {placeholder: 'Примечание', size: 28});
    const form = el('form', {
        class: 'inline',
        onsubmit: async e => {
            e.preventDefault();
            if (await attempt(() => api('POST', 'partners', {name: name.value, phone: phone.value, note: note.value}), 'Клиент добавлен')) {
                route();
            }
        },
    }, name, phone, note, el('button', {class: 'primary'}, 'Добавить'));
    render(view,
        el('h2', {}, 'Клиенты'),
        form,
        el('div', {class: 'toolbar', style: 'margin-top: 12px'},
            el('label', {class: 'check'},
                el('input', {type: 'checkbox', checked: partnersState.archived, onchange: e => { partnersState.archived = e.target.checked; route(); }}),
                'показать архив')),
        el('div', {class: 'table-wrap'}, el('table', {},
            el('thead', {}, el('tr', {}, el('th', {}, 'Клиент'), el('th', {}, 'Телефон'), el('th', {}, 'Примечание'))),
            el('tbody', {}, partners.length
                ? partners.map(p => el('tr', {class: 'link' + (p.archived ? ' archived' : ''), onclick: () => { location.hash = '#/partners/' + p.id; }},
                    el('td', {}, p.name, p.archived ? el('span', {class: 'muted'}, ' (архив)') : null),
                    el('td', {}, p.phone),
                    el('td', {class: 'muted'}, p.note)))
                : el('tr', {}, el('td', {colspan: 3, class: 'muted'}, 'Клиентов нет'))))));
}

async function renderPartner(view, id) {
    const p = await api('GET', `partners/${id}`);
    const name = el('input', {value: p.name, size: 28});
    const phone = el('input', {value: p.phone, size: 16});
    const note = el('input', {value: p.note, size: 40});
    const archived = el('input', {type: 'checkbox', checked: p.archived});
    const save = async () => {
        if (await attempt(() => api('PUT', `partners/${id}`, {name: name.value, phone: phone.value, note: note.value, archived: archived.checked}), 'Сохранено')) {
            route();
        }
    };
    const remove = async () => {
        if (confirm(`Удалить клиента «${p.name}»?`) && await attempt(() => api('DELETE', `partners/${id}`), 'Клиент удалён')) {
            location.hash = '#/partners';
        }
    };
    render(view,
        el('a', {class: 'back', href: '#/partners'}, '← Клиенты'),
        el('h2', {}, p.name),
        el('div', {class: 'fields'},
            el('label', {}, 'Название', name),
            el('label', {}, 'Телефон', phone),
            el('label', {}, 'Примечание', note),
            el('label', {class: 'check'}, archived, 'в архиве'),
            el('button', {class: 'primary', onclick: save}, 'Сохранить'),
            el('button', {class: 'danger', onclick: remove}, 'Удалить')));
}

// ---------- остатки ----------

async function renderStock(view, params) {
    const date = params.get('date') || isoDate(new Date());
    const groupId = params.get('group') ? Number(params.get('group')) : null;
    const groups = await api('GET', 'groups');
    const balances = await api('GET', 'stock?date=' + date + (groupId ? '&group=' + groupId : ''));
    const go = (d, g = groupId) => { location.hash = '#/stock?date=' + d + (g ? '&group=' + g : ''); };
    const groupSelect = el('select', {onchange: e => go(date, selectValue(e.target))}, groupOptions(groups, groupId, 'все группы'));
    render(view,
        el('h2', {}, 'Остатки'),
        el('div', {class: 'toolbar'},
            el('button', {title: 'Предыдущий день', onclick: () => go(shiftDate(date, -1))}, '←'),
            el('input', {type: 'date', value: date, onchange: e => e.target.value && go(e.target.value)}),
            el('button', {title: 'Следующий день', onclick: () => go(shiftDate(date, 1))}, '→'),
            el('span', {class: 'muted'}, 'на конец дня ' + humanDate(date))),
        el('div', {class: 'toolbar'}, 'Группа', groupSelect),
        el('div', {class: 'table-wrap'}, el('table', {class: 'needs'},
            el('thead', {}, el('tr', {}, el('th', {}, 'Продукция'), el('th', {}, 'Группа'), el('th', {class: 'num'}, 'Остаток'), el('th', {}, 'Ед.'))),
            el('tbody', {}, balances.length
                ? balances.map(b => el('tr', {class: b.archived ? 'archived' : null},
                    el('td', {}, b.name, b.archived ? el('span', {class: 'muted'}, ' (архив)') : null),
                    el('td', {class: 'muted'}, b.group_id ? groupPath(groups, b.group_id) : ''),
                    el('td', {class: 'num'}, formatQty(b.qty)),
                    el('td', {}, UNITS[b.unit])))
                : el('tr', {}, el('td', {colspan: 4, class: 'muted'}, 'Остатков нет'))))));
}

// ---------- расчёт потребности ----------

function needsTable(title, needs, note) {
    if (!needs.length) {
        return null;
    }
    return el('section', {},
        el('h3', {}, title),
        note ? el('p', {class: 'muted'}, note) : null,
        el('div', {class: 'table-wrap'}, el('table', {class: 'needs'},
            el('thead', {}, el('tr', {}, el('th', {}, 'Наименование'), el('th', {class: 'num'}, 'Количество'), el('th', {}, 'Ед.'))),
            el('tbody', {}, needs.map(n => el('tr', {},
                el('td', {}, n.name),
                el('td', {class: 'num'}, formatQty(n.qty)),
                el('td', {}, UNITS[n.unit])))))));
}

async function renderRequirements(view, params) {
    const docs = params.get('docs') || '';
    const date = params.get('date');
    const [req, orders] = await Promise.all([
        api('GET', 'requirements?docs=' + encodeURIComponent(docs)),
        Promise.all(docs.split(',').filter(Boolean).map(id => api('GET', 'docs/' + id))),
    ]);
    render(view,
        el('a', {class: 'back no-print', href: '#/orders' + (date ? '?date=' + date : '')}, '← Заявки'),
        el('div', {class: 'toolbar section-head', style: 'margin-top: 0'},
            el('h2', {}, 'Потребность' + (date ? ' на ' + humanDate(date) : '')),
            el('button', {class: 'primary no-print', onclick: () => window.print()}, 'Печать')),
        el('p', {class: 'muted'}, 'Заявки: ' + orders.map(o => `№ ${o.number}${orderFrom(o) ? ' (' + orderFrom(o) + ')' : ''}`).join(', ')),
        req.warnings.length ? el('div', {class: 'warnings'}, req.warnings.map(w => el('div', {}, '⚠ ' + w))) : null,
        needsTable('Произвести', req.products),
        needsTable('Полуфабрикаты', req.intermediates, 'Сколько приготовить, чтобы произвести продукцию выше.'),
        needsTable('Сырьё', req.raw, req.raw.some(n => n.unit === 'pcs') ? 'Штучное сырьё округлено вверх до целых.' : null));
}

// ---------- маршрутизация ----------

async function route() {
    const view = document.getElementById('view');
    const [hash, query] = (location.hash || '#/items').split('?');
    const params = new URLSearchParams(query || '');
    const section = {requirements: 'orders'}[hash.split('/')[1]] || hash.split('/')[1];
    for (const a of document.querySelectorAll('nav a[data-section]')) {
        a.classList.toggle('active', a.dataset.section === section);
    }
    try {
        let m;
        if ((m = hash.match(/^#\/items\/(\d+)$/))) {
            await renderItem(view, Number(m[1]));
        } else if (hash === '#/orders') {
            await renderOrders(view, params);
        } else if (hash === '#/orders/new') {
            params.set('type', 'order');
            await renderDoc(view, null, params);
        } else if ((m = hash.match(/^#\/(?:orders|docs)\/(\d+)$/))) {
            await renderDoc(view, Number(m[1]), params);
        } else if (hash === '#/docs/new') {
            await renderDoc(view, null, params);
        } else if (hash === '#/docs') {
            await renderDocs(view);
        } else if (hash === '#/partners') {
            await renderPartners(view);
        } else if ((m = hash.match(/^#\/partners\/(\d+)$/))) {
            await renderPartner(view, Number(m[1]));
        } else if (hash === '#/prices') {
            await renderPrices(view, params);
        } else if (hash === '#/stock') {
            await renderStock(view, params);
        } else if (hash === '#/requirements') {
            await renderRequirements(view, params);
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
