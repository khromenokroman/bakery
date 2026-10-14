// Пользователи (#9): без входа - форма входа; неверный пароль - ошибка; администратор входит, видит
// «Пользователи» и добавляет продавца Ольгу. Ольга входит: нет «Пользователей» и «Организации», номенклатура
// только для просмотра, установку цен не создать; созданное ей поступление показывает автора. Ольга меняет
// пароль и входит с новым.
const message = () => document.getElementById('message').textContent;
const navText = () => [...document.querySelectorAll('nav a')].filter(a => !a.hidden).map(a => a.textContent).join(',');
const login = async (user, password) => {
    const form = document.querySelector('form.login');
    expect(form, 'нет формы входа');
    form.querySelector('[name=login]').value = user;
    form.querySelector('[name=password]').value = password;
    btn('Войти').click();
    await sleep(800);
};
const logout = async () => { btn('Выйти').click(); await sleep(700); };

await sleep(300);
expect(document.querySelector('form.login') && btn('Войти'), 'без входа должна быть форма входа');
expect(getComputedStyle(document.querySelector('nav')).display === 'none', 'меню разделов без входа скрыто');
await login('admin', 'wrong!');
expect(message().includes('неверный логин или пароль'), 'нет ошибки неверного пароля: ' + message());
await login('admin', 'secret1');
expect(!document.querySelector('form.login'), 'администратор не вошёл: ' + message());
expect(navText().includes('Пользователи'), 'у администратора нет раздела «Пользователи»: ' + navText());
expect(document.getElementById('user-menu').textContent.includes('Админ'), 'в меню нет имени пользователя');
log.push('администратор вошёл');

location.hash = '#/users';
await sleep(700);
const form = document.querySelector('form.inline');
const [loginInput, nameInput] = form.querySelectorAll('input');
loginInput.value = 'olga';
nameInput.value = 'Ольга';
form.querySelector('select').value = 'seller';
form.querySelector('input[type=password]').value = 'secret1';
btn('Добавить').click();
await sleep(800);
expect(document.querySelector('main').textContent.includes('olga'), 'продавец не добавился: ' + message());
log.push('добавлен продавец olga');

await logout();
await login('olga', 'secret1');
expect(!navText().includes('Пользователи') && !navText().includes('Организация'), 'у продавца лишние разделы: ' + navText());
location.hash = '#/items';
await sleep(700);
expect(!btn('Добавить') && !btn('+ группа'), 'продавец не должен добавлять номенклатуру и группы');
const bread = (await (await fetch('/api/items')).json()).find(i => i.name === 'Батон').id;
location.hash = '#/items/' + bread;
await sleep(700);
expect(!btn('Сохранить') && !btn('Загрузить фото'), 'в карточке у продавца есть кнопки изменения');
expect([...document.querySelectorAll('main input')].every(i => i.disabled), 'поля карточки должны быть выключены');
location.hash = '#/docs';
await sleep(700);
expect(btn('Новое поступление') && !btn('Новая установка цен'), 'продавцу: поступление можно, установку цен нельзя');
log.push('продавец: справочники только просмотр, без установки цен');

btn('Новое поступление').click();
await sleep(700);
document.querySelector('tbody select').value = String(bread);
document.querySelector('tbody select').dispatchEvent(new Event('change'));
document.querySelector('tbody input').value = '5';
document.querySelector('tbody input').dispatchEvent(new Event('input'));
btn('Сохранить и провести').click();
await sleep(1000);
const authors = document.querySelector('p.authors');
expect(authors && authors.textContent.startsWith('Создал: Ольга'), 'нет автора документа: ' + (authors ? authors.textContent : message()));
log.push(authors.textContent);

location.hash = '#/password';
await sleep(700);
const [oldPw, newPw, repeatPw] = document.querySelectorAll('main input[type=password]');
oldPw.value = 'secret1';
newPw.value = repeatPw.value = 'olga-new';
btn('Сменить пароль').click();
await sleep(800);
expect(message().includes('Пароль изменён'), 'пароль не сменился: ' + message());
await logout();
await login('olga', 'secret1');
expect(document.querySelector('form.login'), 'старый пароль не должен подходить');
await login('olga', 'olga-new');
expect(!document.querySelector('form.login'), 'не вошла с новым паролем: ' + message());
log.push('пароль сменён, вход с новым');
