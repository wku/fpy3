Вот **чеклист** для реализации специализированного API-сервера **только на HTTP/3 \+ WebTransport** с использованием стека **msquic \+ nghttp3 \+ libwtf** (на языке C).

Чеклист структурирован по этапам, от подготовки до продакшен-готовности. Всё ориентировано на максимальную производительность и минимальный оверхед.

### **1\. Подготовка зависимостей и сборки**

* Скачать и собрать **msquic** (последняя версия с GitHub microsoft/msquic)  
* Собрать msquic с нужными опциями: \-DQUIC\_BUILD\_SHARED=OFF, \-DQUIC\_ENABLE\_LOGGING=ON (для отладки), BBRv2 по умолчанию  
* Скачать и собрать **nghttp3** (nghttp2/nghttp3)  
* Скачать **libwtf** (github.com/andrewmd5/libwtf) — обратить внимание: early development, не production-ready  
* Слинковать все три библиотеки в свой проект (статически предпочтительнее)  
* Настроить CMake / Makefile для кросс-платформенной сборки (Linux/Windows/macOS)

### **2\. Глобальная инициализация**

* Вызвать MsQuicOpen() → получить MsQuic API table  
* Настроить глобальные настройки msquic (MsQuicSetSettings):  
  * Включить 0-RTT resumption  
  * Включить connection migration  
  * Выбрать congestion control: BBRv2 (рекомендуется для максимальной производительности)  
  * Установить idle timeout (например, 30–60 сек)  
* Инициализировать TLS-конфиг (сертификат \+ приватный ключ через MsQuicCredentialConfig)  
* Создать и настроить **libwtf** контекст (wtf\_ctx\_new или аналог из libwtf API)

### **3\. Запуск слушателя (только UDP)**

* Создать UDP-сокет (не TCP\!)  
* Зарегистрировать слушатель: MsQuicListenerOpen \+ MsQuicListenerStart  
* Указать ALPN-лист: только "h3" (без h2, http/1.1)  
* Включить WebTransport support: отправлять SETTINGS\_ENABLE\_WEBTRANSPORT \= 1 (libwtf обычно делает это автоматически — проверить\!)  
* Настроить обработчик новых соединений (CONNECTION\_CONNECTED callback)

### **4\. Обработка соединения (connection level)**

* В CONNECTION\_CONNECTED:  
  * Создать nghttp3\_conn: nghttp3\_conn\_server\_new \+ callbacks (on\_header, on\_data, on\_stream\_close и т.д.)  
  * Привязать nghttp3 к msquic: через nghttp3 callbacks на send/recv  
  * Создать WebTransport сессию через libwtf (wtf\_session или wtf\_transport\_new)  
* Реализовать проверку Origin (Sec-WebTransport-Origin или аналог) — как CORS  
* Настроить обработку ошибок и shutdown: MsQuicConnectionShutdown

### **5\. Обработка стрима и HTTP/3 запросов**

* В STREAM\_START / PEER\_STREAM\_CREATED:  
  * Привязать стрим к nghttp3: nghttp3\_conn\_bind\_stream  
* Для обычных HTTP/3 запросов (не CONNECT):  
  * Собирать заголовки и тело через nghttp3 callbacks  
  * Создавать fpy.Request из HTTP/3 фреймов  
  * Генерировать ответ → nghttp3\_conn\_submit\_response → MsQuicStreamSend  
* Отключить поддержку upgrade / CONNECT для обычных путей (если не нужен)

### **6\. WebTransport сессия (самая важная часть)**

* Обнаружить extended CONNECT:  
  * :method \= CONNECT  
  * :scheme \= https  
  * :path \= /твой\_путь\_для\_wt  
  * Заголовок Sec-WebTransport: ?1  
* Через libwtf подтвердить сессию (wtf\_session\_accept или аналог)  
* Создать внутреннюю структуру FpyWebTransportSession:  
  * Хранить session\_id, msquic connection, wtf\_session, app\_context  
  * Хранить map активных стримов (bidirectional / unidirectional)  
* Реализовать callbacks от libwtf:  
  * on\_session\_established  
  * on\_bidi\_stream\_open (новый bidirectional stream от клиента)  
  * on\_unidi\_stream\_incoming (сервер → клиент unidirectional)  
  * on\_datagram\_received (unreliable datagrams)  
  * on\_stream\_reset / on\_stream\_close  
* Реализовать отправку:  
  * Bidirectional: wtf\_stream\_send / write  
  * Unidirectional (server-initiated): wtf\_session\_create\_unidi\_stream  
  * Datagram: wtf\_session\_send\_datagram

### **7\. Применение к твоей бизнес-логике (fpy.Request адаптация)**

* Для HTTP/3 → классический fpy.Request / fpy.Response (без upgrade)  
* Для WebTransport → новая модель:  
  * Сессионный handler (on\_session\_open / on\_session\_close)  
  * Stream handler (on\_stream\_data / on\_stream\_close)  
  * Datagram handler (очень быстрый путь, без гарантий доставки/порядка)  
* Реализовать backpressure: мониторить flow control msquic и останавливать чтение при переполнении

### **8\. Безопасность и отказы**

* Проверять Origin строго (reject если не разрешён)  
* Обрабатывать все reset/error коды msquic/nghttp3/libwtf  
* Отправлять 403/400/429 при ошибках negotiation  
* Настроить rate limiting на уровне соединения/сессии

### **9\. Тестирование и метрики (обязательно\!)**

* Тесты на Chrome/Edge (WebTransport API)  
* Тесты на потерю пакетов, handover сети, high BDP  
* Метрики:  
  * % 0-RTT соединений  
  * Кол-во стримов/сессию  
  * Datagram loss rate  
  * CPU / throughput под нагрузкой (wrk / h2load с QUIC)  
* Логирование: msquic trace \+ свой app-level log

### **10\. Продакшен-готовность**

* Проверить стабильность libwtf под нагрузкой (если крашится — патчить или перейти на lsquic)  
* Настроить graceful shutdown  
* Добавить мониторинг (Prometheus или свой)  
* Подумать о fallback-плане (отдельный порт с HTTP/2 \+ WS на будущее)

