// Резервные копии (#8): у администратора раздел «Копии»; «Сделать копию сейчас» добавляет копию вручную, ссылка
// «Скачать» отдаёт файл SQLite; расписание сохраняется; удаление с подтверждением. Технологу раздел не виден.
const message = () => document.getElementById('message').textContent;
const rows = () => [...document.querySelectorAll('main tbody tr')].filter(r => r.querySelector('a.button'));
expect([...document.querySelectorAll('nav a')].some(a => !a.hidden && a.textContent === 'Копии'), 'у администратора нет раздела «Копии»');
location.hash = '#/backups';
await sleep(700);
expect(document.querySelector('main').textContent.includes('Копий пока нет'), 'в новой базе копий быть не должно');
btn('Сделать копию сейчас').click();
await sleep(1000);
expect(message().includes('Копия сделана') && rows().length === 1, 'копия не появилась: ' + message());
const link = rows()[0].querySelector('a.button');
const file = await (await fetch(link.href)).arrayBuffer();
const head = new TextDecoder().decode(new Uint8Array(file).slice(0, 15));
expect(head === 'SQLite format 3', 'скачивается не база: ' + head);
expect(rows()[0].textContent.includes('вручную'), 'копия должна быть ручной');
log.push(`копия вручную, скачано ${file.byteLength} байт`);

const [enabled] = document.querySelectorAll('main input[type=checkbox]');
enabled.checked = true;
document.querySelector('main input[type=time]').value = '02:30';
document.querySelector('main input[type=number]').value = '5';
btn('Сохранить').click();
await sleep(800);
const schedule = (await (await fetch('/api/backups')).json()).schedule;
expect(schedule.enabled && schedule.time === '02:30' && schedule.keep === 5, 'расписание не сохранилось: ' + JSON.stringify(schedule));
log.push('расписание: каждый день в 02:30, хранить 5');

btn('Удалить').click();
await sleep(800);
expect(rows().length === 0, 'копия не удалилась: ' + message());
log.push('копия удалена');

await fetch('/api/users', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({login: 'tanya', role: 'technologist', password: 'secret1'})});
await fetch('/api/auth/login', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({login: 'tanya', password: 'secret1'})});
const denied = await fetch('/api/backups');
expect(denied.status === 403, 'технологу API копий должно отказывать: ' + denied.status);
log.push('технологу копии недоступны');
