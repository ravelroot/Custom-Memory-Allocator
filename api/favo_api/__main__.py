import argparse
import os

import uvicorn


def main() -> None:
    parser = argparse.ArgumentParser(prog="favo-api", description="API e painel web do alocador Favo")
    parser.add_argument("--host", default=os.environ.get("FAVO_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("FAVO_PORT", "8080")))
    parser.add_argument("--reload", action="store_true")
    args = parser.parse_args()
    uvicorn.run("favo_api.main:app", host=args.host, port=args.port, reload=args.reload, proxy_headers=True)


if __name__ == "__main__":
    main()
