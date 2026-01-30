from typing import Any, Dict, List, Optional, Protocol, Tuple, Union

class Request:
    method: str
    path: str
    query_string: Optional[str]
    version: str
    headers: Dict[str, str]
    body: Optional[bytes]
    match_dict: Optional[Dict[str, str]]
    transport: Any
    keep_alive: bool
    route: Any
    extra: Dict[str, Any]
    app: Any
    
    def Response(self, code: int = 200, text: Optional[str] = None, 
                 body: Optional[bytes] = None, mime_type: str = 'text/plain; charset=utf-8', 
                 headers: Optional[Dict[str, str]] = None) -> Any: ...
                 
    def add_done_callback(self, callback: Any) -> None: ...

def configure_pool(max_size: int = 1024) -> None: ...
