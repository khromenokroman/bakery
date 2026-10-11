// Группы: создать «Кондитерка» и в ней «Торты», добавить позицию в «Торты», найти её через «Кондитерку»,
// в редакторе «Кондитерки» среди родителей не должно быть её подгруппы «Торты».
const answers = ['Кондитерка', 'Торты'];
window.prompt = () => answers.shift();
const node = text => [...document.querySelectorAll('.tree-node')].find(n => n.textContent === text);

btn('+ группа').click();
await sleep(600);
expect(node('Кондитерка') && node('Кондитерка').classList.contains('active'), 'группа «Кондитерка» не создана или не выбрана');
btn('+ группа').click();
await sleep(600);
expect(document.querySelector('.split-main h3').textContent === 'Кондитерка / Торты', 'заголовок: ' + document.querySelector('.split-main h3').textContent);
log.push('дерево: ' + [...document.querySelectorAll('.tree-node')].map(n => n.textContent).join(', '));

const form = document.querySelector('form.inline');
form.querySelector('input[name=name]').value = 'Наполеон';
form.querySelector('select[name=unit]').value = 'kg';
expect(form.querySelector('select[name=group]').selectedOptions[0].text.trim() === 'Торты', 'в форме не выбрана текущая группа');
form.querySelector('button').click();
await sleep(800);
expect(/^#\/items\/\d+$/.test(location.hash), 'не открылась карточка: ' + location.hash);
const cardGroup = [...document.querySelectorAll('.fields label')].find(l => l.textContent.startsWith('Группа')).querySelector('select');
expect(cardGroup.selectedOptions[0].text.trim() === 'Торты', 'в карточке группа: ' + cardGroup.selectedOptions[0].text);

location.hash = '#/items';
await sleep(600);
node('Кондитерка').click();
await sleep(600);
const rows = [...document.querySelectorAll('.split-main tbody tr')].map(r => r.cells[0].textContent + ' / ' + r.cells[3].textContent);
log.push('в «Кондитерке»: ' + rows.join('; '));
expect(rows.join('; ') === 'Наполеон / Кондитерка / Торты', 'позиция из подгруппы не видна в родительской группе');

btn('Изменить').click();
await sleep(200);
const parent = [...document.querySelectorAll('aside .panel label')].find(l => l.textContent.startsWith('Входит в')).querySelector('select');
expect(![...parent.options].some(o => o.text.trim() === 'Торты'), 'в выборе родителя есть собственная подгруппа «Торты»');
log.push('в выборе родителя нет подгруппы «Торты»');
