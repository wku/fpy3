import signal
import asyncio
import traceback
import socket
import os
import sys
import multiprocessing
import faulthandler

import uvloop

from fpy.router import Router, RouteNotFoundException
from fpy.protocol.cprotocol import Protocol
from fpy.protocol.creaper import Reaper
from fpy.request import crequest
try:
    from fpy.protocol.cquic import QuicServer
except ImportError as e:
    import traceback
    traceback.print_exc()
    QuicServer = None

import logging

import logging

logger = logging.getLogger(__name__)


signames = {
    int(v): v.name for k, v in signal.__dict__.items()
    if isinstance(v, signal.Signals)}


class Application:
    def __init__(self, *, reaper_settings=None, log_request=None,
                 protocol_factory=None, debug=False, max_requests=1024,
                 enable_http3=False):
        crequest.configure_pool(max_size=max_requests)
        self._enable_http3 = enable_http3
        self._router = None
        self._loop = None
        self._connections = set()
        self._reaper_settings = reaper_settings or {}
        self._error_handlers = []
        self._log_request = log_request
        self._request_extensions = {}
        self._protocol_factory = protocol_factory or Protocol
        self._debug = debug

    @property
    def loop(self):
        if not self._loop:
            self._loop = uvloop.new_event_loop()

        return self._loop

    @property
    def router(self):
        if not self._router:
            self._router = Router()

        return self._router

    def __finalize(self):
        self.loop
        self.router

        self._reaper = Reaper(self, **self._reaper_settings)
        self._matcher = self._router.get_matcher()

    def protocol_error_handler(self, error):
        logger.error(f"Protocol error: {error}")

        error = error.encode('utf-8')

        response = [
            'HTTP/1.0 400 Bad Request\r\n',
            'Content-Type: text/plain; charset=utf-8\r\n',
            'Content-Length: {}\r\n\r\n'.format(len(error))]

        return ''.join(response).encode('utf-8') + error

    def default_request_logger(self, request):
        logger.info(f"{request.remote_addr} {request.method} {request.path}")

    def add_error_handler(self, typ, handler):
        self._error_handlers.append((typ, handler))

    def default_error_handler(self, request, exception):
        if isinstance(exception, RouteNotFoundException):
            return request.Response(code=404, text='Not Found')
        if isinstance(exception, asyncio.CancelledError):
            return request.Response(code=503, text='Service unavailable')

        tb = ''.join(tb)
        logger.error(tb)
        return request.Response(
            code=500,
            text=tb if self._debug else 'Internal Server Error')

    def error_handler(self, request, exception):
        for typ, handler in self._error_handlers:
            if typ is not None and not isinstance(exception, typ):
                continue

            try:
                return handler(request, exception)
            except:
                logger.error('-- Exception in error_handler occured:')
                logger.error(traceback.format_exc())

            logger.error('-- while handling:')
            logger.error(traceback.format_exception(None, exception, exception.__traceback__))
            return request.Response(
                code=500, text='Internal Server Error')

        return self.default_error_handler(request, exception)

    def _get_idle_and_busy_connections(self):
        return \
            [c for c in self._connections if c.pipeline_empty], \
            [c for c in self._connections if not c.pipeline_empty]

    async def drain(self):
        idle, busy = self._get_idle_and_busy_connections()
        for c in idle:
            c.transport.close()

        if idle or busy:
            logger.info('Draining connections...')
        else:
            return

        if idle:
            logger.info('{} idle connections closed immediately'.format(len(idle)))
        if busy:
            logger.info('{} connections busy, read-end closed'.format(len(busy)))

        for x in range(5, 0, -1):
            await asyncio.sleep(1)
            idle, busy = self._get_idle_and_busy_connections()
            for c in idle:
                c.transport.close()
            if not busy:
                break
            else:
                logger.info(
                    "{} seconds remaining, {} connections still busy"
                    .format(x, len(busy)))

        _, busy = self._get_idle_and_busy_connections()
        if busy:
            logger.info('Forcefully killing {} connections'.format(len(busy)))
        for c in busy:
            c.pipeline_cancel()

    def extend_request(self, handler, *, name=None, property=False):
        if not name:
            name = handler.__name__

        self._request_extensions[name] = (handler, property)

    def serve(self, *, sock, host, port, reloader_pid):
        faulthandler.enable()
        self.__finalize()

        loop = self.loop
        asyncio.set_event_loop(loop)

        if self._enable_http3 and QuicServer:
            logger.info(f'Starting QUIC Listener on {host}:{port}')
            self._quic_server = QuicServer(self, self.loop)
            self._quic_server.start(host, port)
        elif self._enable_http3 and not QuicServer:
             logger.warning('HTTP/3 enabled but cquic module not available.')

        server = None
        if sock:
            server_coro = loop.create_server(
                lambda: self._protocol_factory(self), sock=sock)
            server = loop.run_until_complete(server_coro)

        loop.add_signal_handler(signal.SIGTERM, loop.stop)
        loop.add_signal_handler(signal.SIGINT, loop.stop)

        if reloader_pid:
            from fpy.reloader import ChangeDetector
            detector = ChangeDetector(loop)
            detector.start()

        logger.info('Accepting connections on http://{}:{}'.format(host, port))

        try:
            loop.run_forever()
        finally:
            if server:
                server.close()
                loop.run_until_complete(server.wait_closed())
            loop.run_until_complete(self.drain())
            self._reaper.stop()
            loop.close()

            # break reference and cleanup matcher buffer
            del self._matcher

    def _run(self, *, host, port, worker_num=None, reloader_pid=None,
             debug=None):
        self._debug = debug or self._debug
        if self._debug and not self._log_request:
            self._log_request = self._debug

        sock = None
        if not self._enable_http3:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind((host, port))
            os.set_inheritable(sock.fileno(), True)

        workers = set()

        terminating = False

        def stop(sig, frame):
            nonlocal terminating
            if reloader_pid and sig == signal.SIGHUP:
                logger.info('Reload request received')
            elif not terminating:
                terminating = True
                logger.info('Termination request received')
            for worker in workers:
                worker.terminate()

        signal.signal(signal.SIGINT, stop)
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGHUP, stop)

        for _ in range(worker_num or 1):
            worker = multiprocessing.Process(
                target=self.serve,
                kwargs=dict(sock=sock, host=host, port=port,
                            reloader_pid=reloader_pid))
            worker.daemon = True
            worker.start()
            workers.add(worker)

        # prevent further operations on socket in parent
        if sock:
            sock.close()

        for worker in workers:
            worker.join()

            if worker.exitcode > 0:
                logger.warning('Worker exited with code {}'.format(worker.exitcode))
            elif worker.exitcode < 0:
                try:
                    signame = signames[-worker.exitcode]
                except KeyError:
                    logger.error(
                        'Worker crashed with unknown code {}!'
                        .format(worker.exitcode))
                else:
                    logger.error('Worker crashed on signal {}!'.format(signame))

    def run(self, host='0.0.0.0', port=8080, *, worker_num=None, reload=False,
            debug=False):
        if os.environ.get('_FPY_IGNORE_RUN'):
            return

        reloader_pid = None
        if reload:
            if '_FPY_RELOADER' not in os.environ:
                from fpy.reloader import exec_reloader
                exec_reloader(host=host, port=port, worker_num=worker_num)
            else:
                reloader_pid = int(os.environ['_FPY_RELOADER'])

        self._run(
            host=host, port=port, worker_num=worker_num,
            reloader_pid=reloader_pid, debug=debug)
