use serde::Serialize;
use sqlx::{Connection, Row};

use super::{LibraryDatabase, LibraryError};
use crate::MutationControl;

const DEFAULT_SETTINGS_REVISION: u64 = 1;

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "lowercase")]
pub(crate) enum Appearance {
    Light,
    Dark,
    Cozy,
}

impl Appearance {
    fn stored(self) -> &'static str {
        match self {
            Self::Light => "light",
            Self::Dark => "dark",
            Self::Cozy => "cozy",
        }
    }

    fn from_stored(value: &str) -> Result<Self, LibraryError> {
        match value {
            "light" => Ok(Self::Light),
            "dark" => Ok(Self::Dark),
            "cozy" => Ok(Self::Cozy),
            _ => Err(LibraryError::Database(
                "stored appearance setting is invalid".to_string(),
            )),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct AppearanceSettings {
    pub(crate) revision: u64,
    pub(crate) appearance: Appearance,
}

pub(crate) async fn load(
    connection: &mut sqlx::SqliteConnection,
) -> Result<AppearanceSettings, LibraryError> {
    let rows = sqlx::query(
        "SELECT key, value
         FROM app_settings
         WHERE key IN ('appearance', 'appearanceRevision')
         ORDER BY key",
    )
    .fetch_all(&mut *connection)
    .await?;
    if rows.is_empty() {
        return Ok(AppearanceSettings {
            revision: DEFAULT_SETTINGS_REVISION,
            appearance: Appearance::Light,
        });
    }
    if rows.len() != 2 {
        return Err(LibraryError::Database(
            "appearance settings are incomplete".to_string(),
        ));
    }

    let mut appearance = None;
    let mut revision = None;
    for row in rows {
        match row.try_get::<String, _>("key")?.as_str() {
            "appearance" => {
                appearance = Some(Appearance::from_stored(
                    &row.try_get::<String, _>("value")?,
                )?)
            }
            "appearanceRevision" => {
                revision = Some(
                    row.try_get::<String, _>("value")?
                        .parse::<u64>()
                        .ok()
                        .filter(|revision| *revision != 0)
                        .ok_or_else(|| {
                            LibraryError::Database(
                                "stored appearance revision is invalid".to_string(),
                            )
                        })?,
                )
            }
            _ => unreachable!("settings query returned an unrequested key"),
        }
    }
    Ok(AppearanceSettings {
        revision: revision.expect("validated settings revision row"),
        appearance: appearance.expect("validated appearance row"),
    })
}

impl LibraryDatabase {
    pub(crate) fn appearance_settings(&self) -> AppearanceSettings {
        self.appearance_settings
    }

    pub(crate) async fn put_appearance(
        &mut self,
        expected_revision: u64,
        appearance: Appearance,
        max_payload_bytes: usize,
        control: &MutationControl,
    ) -> Result<AppearanceSettings, LibraryError> {
        if expected_revision != self.appearance_settings.revision {
            return Err(LibraryError::StaleSettingsRevision {
                expected: expected_revision,
                actual: self.appearance_settings.revision,
            });
        }
        if appearance == self.appearance_settings.appearance {
            return Ok(self.appearance_settings);
        }
        if control.is_cancelled() {
            return Err(LibraryError::Cancelled);
        }
        let revision = expected_revision
            .checked_add(1)
            .ok_or(LibraryError::SettingsRevisionExhausted)?;
        let updated = AppearanceSettings {
            revision,
            appearance,
        };
        if serde_json::to_vec(&updated)
            .map_err(|error| LibraryError::Database(error.to_string()))?
            .len()
            > max_payload_bytes
        {
            return Err(LibraryError::ResponseTooLarge {
                limit: max_payload_bytes,
            });
        }

        let mut transaction = self.connection.begin_with("BEGIN IMMEDIATE").await?;
        sqlx::query(
            "INSERT INTO app_settings (key, value, updated_at)
             VALUES ('appearance', ?1, CURRENT_TIMESTAMP)
             ON CONFLICT(key) DO UPDATE SET
                 value = excluded.value,
                 updated_at = excluded.updated_at",
        )
        .bind(appearance.stored())
        .execute(&mut *transaction)
        .await?;
        sqlx::query(
            "INSERT INTO app_settings (key, value, updated_at)
             VALUES ('appearanceRevision', ?1, CURRENT_TIMESTAMP)
             ON CONFLICT(key) DO UPDATE SET
                 value = excluded.value,
                 updated_at = excluded.updated_at",
        )
        .bind(revision.to_string())
        .execute(&mut *transaction)
        .await?;
        if control.is_cancelled() || !control.begin_commit() {
            transaction.rollback().await?;
            return Err(LibraryError::Cancelled);
        }
        transaction.commit().await?;
        self.appearance_settings = updated;
        Ok(updated)
    }
}
