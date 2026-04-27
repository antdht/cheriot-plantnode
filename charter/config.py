from typing import Annotated, Optional

from pydantic import Field, BaseModel, SecretStr
from pydantic_settings import (BaseSettings, PydanticBaseSettingsSource, TomlConfigSettingsSource,
                               SettingsConfigDict)


class MqttConfig(BaseModel):
    """
    Configuration structure for MQTT connection settings.
    """

    host: str
    port: Annotated[int, Field(default=1883)]
    username: str
    password: SecretStr
    jwt_secret: Annotated[Optional[SecretStr], Field(default=None, validation_alias="jwt-secret")]
    client_id: Annotated[str, Field(validation_alias="client-id")]
    topic_prefix: Annotated[str, Field(default="esp-sensors", validation_alias="topic-prefix")]
    key_topic: Annotated[str, Field(default="key", validation_alias="key-topic")]


class LoggerConfig(BaseModel):
    """
    Configuration structure for Logger settings.
    """

    csv_file_name: Annotated[str, Field(validation_alias="csv-file-name")]


class PlotterConfig(BaseModel):
    """
    Configuration structure for Plotter settings.
    """

    refresh_interval: Annotated[int, Field(default=1000, validation_alias="refresh-interval")]
    """Refresh interval in milliseconds."""

    max_points: Annotated[Optional[int], Field(default=None, validation_alias="max-plot-points")]
    """Maximum number of points to retain in the plots."""


class AppConfig(BaseSettings):
    """
    Configuration structure for application settings.
    """

    model_config = SettingsConfigDict(toml_file="config.toml")

    @classmethod
    def settings_customise_sources(
            cls,
            settings_cls: type[BaseSettings],
            init_settings: PydanticBaseSettingsSource,
            env_settings: PydanticBaseSettingsSource,
            dotenv_settings: PydanticBaseSettingsSource,
            file_secret_settings: PydanticBaseSettingsSource,
    ) -> tuple[PydanticBaseSettingsSource, ...]:
        return (TomlConfigSettingsSource(settings_cls),)

    mqtt: MqttConfig
    logger: LoggerConfig
    plotter: PlotterConfig
