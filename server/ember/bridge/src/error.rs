//! The only error shape the bridge returns (spec 19.5).
use std::borrow::Cow;

use axum::{
    http::{HeaderValue, StatusCode, header},
    response::{IntoResponse, Response},
};
use ember_protocol::api::{ApiError, ErrorBody, ErrorCode};

use crate::util::new_id;

#[derive(Debug)]
pub struct ApiFailure {
    pub code: ErrorCode,
    pub message: Cow<'static, str>,
    pub details: serde_json::Map<String, serde_json::Value>,
    pub retry_after: Option<u64>,
}

impl ApiFailure {
    pub fn new(code: ErrorCode, message: impl Into<Cow<'static, str>>) -> Self {
        Self {
            code,
            message: message.into(),
            details: serde_json::Map::new(),
            retry_after: None,
        }
    }

    pub fn detail(mut self, key: &str, value: impl Into<serde_json::Value>) -> Self {
        self.details.insert(key.into(), value.into());
        self
    }

    pub fn invalid(message: impl Into<Cow<'static, str>>) -> Self {
        Self::new(ErrorCode::InvalidRequest, message)
    }

    pub fn unauthenticated() -> Self {
        Self::new(ErrorCode::Unauthenticated, "Authentication is required.")
    }

    pub fn forbidden() -> Self {
        Self::new(
            ErrorCode::Forbidden,
            "This credential cannot perform that operation.",
        )
    }

    /// Also used for objects that exist but are not visible to the caller,
    /// so existence does not leak.
    pub fn not_found() -> Self {
        Self::new(ErrorCode::NotFound, "Not found.")
    }

    pub fn unavailable() -> Self {
        Self::new(
            ErrorCode::ServiceUnavailable,
            "The bridge could not complete the request.",
        )
    }

    pub fn rate_limited(retry_after: u64) -> Self {
        let mut failure = Self::new(
            ErrorCode::RateLimited,
            "Too many requests. Try again later.",
        );
        failure.retry_after = Some(retry_after.max(1));
        failure
    }
}

impl From<rusqlite::Error> for ApiFailure {
    fn from(error: rusqlite::Error) -> Self {
        // SQL errors name tables and constraints, never bound values.
        eprintln!("ember-bridge: database error: {error}");
        Self::unavailable()
    }
}

impl From<ember_protocol::Error> for ApiFailure {
    fn from(error: ember_protocol::Error) -> Self {
        Self::invalid(error.to_string())
    }
}

impl IntoResponse for ApiFailure {
    fn into_response(self) -> Response {
        let status =
            StatusCode::from_u16(self.code.status()).unwrap_or(StatusCode::INTERNAL_SERVER_ERROR);
        let body = ApiError {
            error: ErrorBody {
                code: self.code,
                message: self.message.into_owned(),
                retryable: self.code.retryable(),
                request_id: new_id("req"),
                details: self.details,
            },
        };
        let mut response = (
            status,
            [(header::CONTENT_TYPE, "application/json")],
            serde_json::to_vec(&body).unwrap_or_default(),
        )
            .into_response();
        if let Some(seconds) = self.retry_after
            && let Ok(value) = HeaderValue::from_str(&seconds.to_string())
        {
            response.headers_mut().insert(header::RETRY_AFTER, value);
        }
        response
    }
}

pub type Result<T> = std::result::Result<T, ApiFailure>;
