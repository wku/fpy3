from typing import Any, Dict, Optional, Union

class Response:
    code: int
    text: Optional[str]
    body: Optional[bytes]
    mime_type: str
    headers: Dict[str, str]
    
    def __init__(self, code: int = 200, text: Optional[str] = None,
                 body: Optional[bytes] = None, mime_type: str = 'text/plain; charset=utf-8',
                 headers: Optional[Dict[str, str]] = None) -> None: ...
