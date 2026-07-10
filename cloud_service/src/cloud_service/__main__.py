import uvicorn

from .config import Settings


def main() -> None:
    settings = Settings.from_env()
    uvicorn.run(
        "cloud_service.main:app",
        host=settings.api_host,
        port=settings.api_port,
    )


if __name__ == "__main__":
    main()
