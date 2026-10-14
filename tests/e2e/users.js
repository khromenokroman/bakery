// Пользователи (#9): без входа - форма входа; неверный пароль - ошибка; администратор входит, видит
// «Пользователи» и добавляет продавца Ольгу. Ольга входит: нет «Пользователей» и «Организации», номенклатура
// только для просмотра, раздела «Цены» и установок цен нет; созданное ей поступление показывает автора. Ольга меняет
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

// администратор вышел со страницы «Пользователи»: продавец попадает на стартовую страницу, а не туда (#14)
expect(location.hash === '#/users', 'перед выходом администратор на «Пользователях»: ' + location.hash);
await logout();
expect(location.hash === '#/' || location.hash === '', 'выход сбрасывает адрес: ' + location.hash);
await login('olga', 'secret1');
expect(location.hash === '#/docs', 'после входа - «Документы», а не страница прошлого пользователя: ' + location.hash);
expect(!message().includes('недостаточно прав') && document.querySelector('main h2').textContent === 'Документы',
    'продавец не должен видеть ошибку прав после входа: ' + message());
log.push('после выхода администратора продавец попал в «Документы» без ошибки');
expect(!navText().includes('Пользователи') && !navText().includes('Организация') && !navText().includes('Цены'), 'у продавца лишние разделы: ' + navText());
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
expect(!btn('Установка цен'), 'в фильтре продавца не должно быть установки цен');
const pricesDoc = await (await fetch('/api/docs?type=prices')).json();
expect(Array.isArray(pricesDoc) && pricesDoc.length === 0, 'API не должно отдавать продавцу установки цен');
location.hash = '#/prices';
await sleep(700);
expect(message().includes('недостаточно прав'), 'раздел «Цены» по адресу должен отказывать продавцу: ' + message());
location.hash = '#/docs';
await sleep(700);
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

// вход, когда в адресе страница, на которую у пользователя нет прав (истекла сессия, ссылка администратора) (#14)
await logout();
location.hash = '#/backups';
await sleep(300);
await login('olga', 'olga-new');
expect(location.hash === '#/docs' && !message().includes('недостаточно прав'),
    `продавец вошёл с адреса «Копий»: ${location.hash}, ${message()}`);
log.push('вход с адреса чужого раздела открывает «Документы»');
