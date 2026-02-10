"""Module entrypoint for running the web client."""

import uvicorn

from .app import create_app


def main() -> None:
    """Run the web client with a minimal development server."""
    app = create_app()
    uvicorn.run(app, host="0.0.0.0", port=8000)


if __name__ == "__main__":
    main()
