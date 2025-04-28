import enum
from typing import Callable, List, Optional, Tuple

from pydantic import BaseModel

from dlslime import _slime_c


class Assignment(BaseModel):
    mr_key: str
    target_offset: int
    source_offset: int
    length: int
