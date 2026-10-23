import pytest
from fastapi.testclient import TestClient

from favo_api.app import create_app


@pytest.fixture()
def client(tmp_path):
    app = create_app(database=str(tmp_path / "favo.db"), web_dir=str(tmp_path / "web"))
    with TestClient(app) as c:
        yield c
