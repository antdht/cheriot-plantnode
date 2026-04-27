from typing import Annotated, Optional

from pydantic import Field, BaseModel
from pydantic_settings import (BaseSettings, PydanticBaseSettingsSource, TomlConfigSettingsSource,
                               SettingsConfigDict)


class MqttConfig(BaseModel):
    host: str
    port: Annotated[int, Field(default=8883)]
    ca_cert: Annotated[str, Field(validation_alias="ca-cert")]
    client_id: Annotated[str, Field(validation_alias="client-id")]
    topic_prefix: Annotated[str, Field(default="plantnode", validation_alias="topic-prefix")]


class LoggerConfig(BaseModel):
    csv_file_name: Annotated[str, Field(validation_alias="csv-file-name")]


class PlotterConfig(BaseModel):
    refresh_interval: Annotated[int, Field(default=1000, validation_alias="refresh-interval")]
    max_points: Annotated[Optional[int], Field(default=None, validation_alias="max-plot-points")]


class AppConfig(BaseSettings):
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
