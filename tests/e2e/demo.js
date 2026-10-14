// Демонстрационная база (#13): на экране первого запуска «Попробовать на демонстрационной базе» заполняет
// пустую базу данными за год и входит под администратором; вверху полоса «Демонстрационная база»;
// в документах сотни реализаций с суммами, в товарном отчёте за год по рознице есть переоценки при продаже;
// после выхода форма входа подсказывает логины, продавец входит с паролем demo123.
const message = () => document.getElementById('message').textContent;
const offer = btn('Попробовать на демонстрационной базе');
expect(offer && btn('Создать администратора'), 'на экране первого запуска нет предложения демо-базы');
const started = Date.now();
offer.click();
await sleep(500);
expect(offer.disabled && offer.textContent.includes('Заполняю'), 'кнопка должна показывать, что идёт заполнение');
while (document.querySelector('form.login') && Date.now() - started < 170000) {
    await sleep(1000);
}
expect(!document.querySelector('form.login'), 'демо-база не заполнилась: ' + message());
await sleep(1500);
log.push(`заполнено за ${Math.round((Date.now() - started) / 1000)} с`);

const banner = document.getElementById('demo-banner');
expect(!banner.hidden && banner.textContent.includes('Демонстрационная база'), 'нет полосы демонстрационной базы');
expect(banner.querySelector('a').href.includes('#сброс-к-чистому-состоянию') || banner.querySelector('a').href.includes('%D1%81%D0%B1%D1%80%D0%BE%D1%81'),
    'ссылка полосы должна вести на сброс в README: ' + banner.querySelector('a').href);
expect(document.getElementById('user-menu').textContent.includes('Администратор'), 'вход не под администратором');

const today = isoDate(new Date());
const docs = await (await fetch(`/api/docs?from=${today.slice(0, 8)}01&to=${today}`)).json();
expect(docs.length > 300, 'документов за месяц мало: ' + docs.length);
location.hash = `#/docs?from=${today}&to=${today}`;
await sleep(1500);
const rows = document.querySelectorAll('main tbody tr').length;
expect(rows >= 10, 'сегодняшних документов в списке: ' + rows);
const items = await (await fetch('/api/items?archived=1')).json();
const partners = await (await fetch('/api/partners?archived=1')).json();
expect(items.length >= 300 && partners.length === 100, `номенклатура ${items.length}, клиенты ${partners.length}`);
log.push(`позиций ${items.length}, клиентов ${partners.length}, документов за месяц ${docs.length}`);

const retail = (await (await fetch('/api/price-types')).json()).find(t => t.name === 'Розница').id;
const report = await (await fetch(`/api/report/goods?from=${shiftDate(today, -364)}&to=${today}&price_type=${retail}`)).json();
expect(report.closing_sum === report.opening_sum + report.in_sum - report.out_sum, 'товарный отчёт за год не сходится');
expect([...report.incoming, ...report.outgoing].some(r => r.revaluation), 'нет переоценок при продаже');
expect(report.lines_without_cost === 0, 'движения без себестоимости: ' + report.lines_without_cost);
log.push(`отчёт за год: остаток ${report.closing_sum / 100} ₽ по себестоимости`);

btn('Выйти').click();
await sleep(800);
expect(document.querySelector('form.login').textContent.includes('demo123'), 'форма входа должна подсказывать демо-логины');
const form = document.querySelector('form.login');
form.querySelector('[name=login]').value = 'seller';
form.querySelector('[name=password]').value = 'demo123';
btn('Войти').click();
await sleep(1200);
expect(document.getElementById('user-menu').textContent.includes('продавец'), 'продавец не вошёл: ' + message());
log.push('продавец вошёл по подсказке');
