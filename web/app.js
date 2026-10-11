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

    render(view,
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

    render(view,
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
            el('td', {}, doc.comment || el('span', {class: 'muted'}, 'без комментария')),
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
        title: 'Заявка', create: 'Новая заявка', comment: 'От кого', placeholder: 'магазин, имя, телефон',
        back: d => '#/orders?date=' + d, backTitle: '← Заявки', posting: false, fill: false,
    },
    production: {
        title: 'Поступление из производства', create: 'Новое поступление', comment: 'Комментарий', placeholder: '',
        back: () => '#/docs', backTitle: '← Документы', posting: true, fill: true,
    },
};

/**
 * Карточка документа; id = null для нового (вид берётся из params.type, по умолчанию заявка).
 */
async function renderDoc(view, id, params) {
    const [doc, products] = await Promise.all([
        id ? api('GET', `docs/${id}`) : Promise.resolve(null),
        api('GET', 'items?kind=product&archived=1'),
    ]);
    const type = doc ? doc.type : (params.get('type') || 'order');
    const cfg = DOC_TYPES[type];
    if (!cfg) {
        throw new Error('неизвестный вид документа: ' + type);
    }
    const posted = doc && doc.status === 'posted';
    const inDoc = new Set((doc ? doc.lines : []).map(l => l.item_id));
    const candidates = products.filter(p => !p.archived || inDoc.has(p.id));
    const byId = new Map(products.map(p => [p.id, p]));
    const date = doc ? doc.date : (params.get('date') || isoDate(new Date()));

    const dateInput = el('input', {type: 'date', value: date});
    const comment = el('input', {value: doc ? doc.comment : '', size: 40, placeholder: cfg.placeholder});
    const tbody = el('tbody');

    const addRow = line => {
        const select = el('select', {}, el('option', {value: ''}, '— выберите —'),
            candidates.map(p => el('option', {value: p.id, selected: line && p.id === line.item_id}, p.name + (p.archived ? ' (архив)' : ''))));
        const unitCell = el('td');
        const updateUnit = () => {
            const p = byId.get(Number(select.value));
            unitCell.textContent = p ? UNITS[p.unit] : '';
        };
        select.addEventListener('change', updateUnit);
        updateUnit();
        const row = el('tr', {},
            el('td', {}, select),
            el('td', {class: 'num'}, el('input', {class: 'qty', value: line ? formatQty(line.qty) : ''})),
            unitCell,
            el('td', {}, el('button', {class: 'danger', title: 'Убрать строку', onclick: () => row.remove()}, '✕')));
        tbody.append(row);
        return row;
    };
    (doc ? doc.lines : []).forEach(addRow);
    if (!doc) {
        addRow(null);
    }

    /** Строки из таблицы; null и сообщение, если что-то не так. */
    const collect = () => {
        const lines = [];
        for (const row of tbody.children) {
            const itemId = Number(row.querySelector('select').value);
            const qtyText = row.querySelector('input').value;
            if (!itemId && !qtyText.trim()) {
                continue;
            }
            if (!itemId) {
                showMessage('В строке не выбрана продукция');
                return null;
            }
            const qty = parseQty(qtyText);
            if (qty === null) {
                showMessage(`«${byId.get(itemId).name}»: введите количество, не более трёх знаков после запятой`);
                return null;
            }
            lines.push({item_id: itemId, qty});
        }
        return lines;
    };

    const save = async andPost => {
        const lines = collect();
        if (!lines) {
            return;
        }
        const body = {type, date: dateInput.value, comment: comment.value, lines};
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
            req.products.forEach(p => addRow({item_id: p.item_id, qty: p.qty}));
            if (!comment.value.trim()) {
                comment.value = `по заявкам № ${chosen.map(o => o.number).join(', ')} на ${humanDate(fillDate).split(',')[0]}`;
            }
            fillPanel.hidden = true;
            showMessage('Строки заполнены по заявкам, проверьте количества', 'ok');
        };
        fillPanel.replaceChildren(
            el('div', {class: 'toolbar'}, el('strong', {}, 'Заявки на'), datePick,
                el('button', {onclick: () => { fillPanel.hidden = true; }}, 'Закрыть')),
            orders.length
                ? el('div', {}, checks.map(([o, c]) => el('label', {class: 'check'}, c,
                    `№ ${o.number}${o.comment ? ' (' + o.comment + ')' : ''}: ${lineSummary(o.lines)}`)))
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
            el('label', {}, cfg.comment, comment)),
        fillPanel,
        el('div', {class: 'table-wrap', style: 'margin-top: 12px'}, el('table', {},
            el('thead', {}, el('tr', {}, el('th', {}, 'Продукция'), el('th', {class: 'num'}, 'Количество'), el('th', {}, 'Ед.'), el('th'))),
            tbody)),
        el('div', {class: 'toolbar', style: 'margin-top: 8px'},
            el('button', {onclick: () => addRow(null).querySelector('select').focus()}, '+ строка')));
}

// ---------- документы ----------

const docsState = {from: null, to: null};

async function renderDocs(view) {
    const today = isoDate(new Date());
    const from = docsState.from || shiftDate(today, -7);
    const to = docsState.to || today;
    const docs = await api('GET', `docs?type=production&from=${from}&to=${to}`);
    const fromInput = el('input', {type: 'date', value: from, onchange: e => { docsState.from = e.target.value; route(); }});
    const toInput = el('input', {type: 'date', value: to, onchange: e => { docsState.to = e.target.value; route(); }});

    const rows = docs.slice().reverse().map(doc => el('tr', {class: 'link', onclick: () => { location.hash = '#/docs/' + doc.id; }},
        el('td', {}, humanDate(doc.date)),
        el('td', {}, `${DOC_TYPES[doc.type].title} № ${doc.number}`),
        el('td', {}, el('span', {class: 'status ' + doc.status}, doc.status === 'posted' ? 'проведён' : 'не проведён')),
        el('td', {}, doc.comment),
        el('td', {}, lineSummary(doc.lines))));

    render(view,
        el('h2', {}, 'Документы'),
        el('div', {class: 'toolbar'},
            el('button', {class: 'primary', onclick: () => { location.hash = '#/docs/new?type=production'; }}, 'Новое поступление')),
        el('div', {class: 'toolbar'}, 'с', fromInput, 'по', toInput),
        el('div', {class: 'table-wrap'}, el('table', {},
            el('thead', {}, el('tr', {}, el('th', {}, 'Дата'), el('th', {}, 'Документ'), el('th', {}, 'Статус'), el('th', {}, 'Комментарий'), el('th', {}, 'Состав'))),
            el('tbody', {}, rows.length ? rows : el('tr', {}, el('td', {colspan: 5, class: 'muted'}, 'За эти дни документов нет'))))));
}

// ---------- остатки ----------

async function renderStock(view, params) {
    const date = params.get('date') || isoDate(new Date());
    const balances = await api('GET', 'stock?date=' + date);
    const go = d => { location.hash = '#/stock?date=' + d; };
    render(view,
        el('h2', {}, 'Остатки'),
        el('div', {class: 'toolbar'},
            el('button', {title: 'Предыдущий день', onclick: () => go(shiftDate(date, -1))}, '←'),
            el('input', {type: 'date', value: date, onchange: e => e.target.value && go(e.target.value)}),
            el('button', {title: 'Следующий день', onclick: () => go(shiftDate(date, 1))}, '→'),
            el('span', {class: 'muted'}, 'на конец дня ' + humanDate(date))),
        el('div', {class: 'table-wrap'}, el('table', {class: 'needs'},
            el('thead', {}, el('tr', {}, el('th', {}, 'Продукция'), el('th', {class: 'num'}, 'Остаток'), el('th', {}, 'Ед.'))),
            el('tbody', {}, balances.length
                ? balances.map(b => el('tr', {class: b.archived ? 'archived' : null},
                    el('td', {}, b.name, b.archived ? el('span', {class: 'muted'}, ' (архив)') : null),
                    el('td', {class: 'num'}, formatQty(b.qty)),
                    el('td', {}, UNITS[b.unit])))
                : el('tr', {}, el('td', {colspan: 3, class: 'muted'}, 'Остатков нет'))))));
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
        el('p', {class: 'muted'}, 'Заявки: ' + orders.map(o => `№ ${o.number}${o.comment ? ' (' + o.comment + ')' : ''}`).join(', ')),
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
