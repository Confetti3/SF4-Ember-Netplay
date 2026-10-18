//! UTC calendar arithmetic: the month a request is counted in, and the
//! RFC 3339 times of a tournament's window. Howard Hinnant's civil-date
//! algorithms, so no date crate is needed for this much.

/// Days since 1970-01-01 of a proleptic Gregorian date.
fn days_from_civil(year: i64, month: i64, day: i64) -> i64 {
    let year = if month <= 2 { year - 1 } else { year };
    let era = year.div_euclid(400);
    let year_of_era = year.rem_euclid(400);
    let day_of_year = (153 * (month + if month > 2 { -3 } else { 9 }) + 2) / 5 + day - 1;
    let day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    era * 146_097 + day_of_era - 719_468
}

/// The date of a day since 1970-01-01: year, month and day.
fn civil_from_days(days: i64) -> (i64, i64, i64) {
    let days = days + 719_468;
    let era = days.div_euclid(146_097);
    let day_of_era = days.rem_euclid(146_097);
    let year_of_era =
        (day_of_era - day_of_era / 1_460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let shifted_month = (5 * day_of_year + 2) / 153;
    let day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
    let month = if shifted_month < 10 {
        shifted_month + 3
    } else {
        shifted_month - 9
    };
    (year_of_era + era * 400 + i64::from(month <= 2), month, day)
}

/// `YYYY-MM` of a Unix time.
pub fn month_key(unix: u64) -> String {
    let (year, month, _) = civil_from_days((unix / 86_400) as i64);
    format!("{year:04}-{month:02}")
}

/// A Unix time from RFC 3339 in UTC: `2026-10-18T19:00:00Z`, with optional
/// fractional seconds (dropped), and `+00:00` for `Z`. Other offsets are
/// refused so a window is never read in the wrong zone.
pub fn parse_utc(text: &str) -> Option<u64> {
    let local = text
        .strip_suffix('Z')
        .or_else(|| text.strip_suffix("+00:00"))?;
    let (date, time) = local.split_once('T')?;
    let (time, fraction) = time.split_once('.').unwrap_or((time, "0"));
    let number = |part: &str, width: usize| {
        (part.len() == width && part.bytes().all(|byte| byte.is_ascii_digit()))
            .then(|| part.parse::<i64>().ok())
            .flatten()
    };
    let fields = |text: &str, separator: char, widths: [usize; 3]| -> Option<[i64; 3]> {
        let mut parts = text.split(separator);
        let values = widths.map(|width| parts.next().and_then(|part| number(part, width)));
        match (values, parts.next()) {
            ([Some(a), Some(b), Some(c)], None) => Some([a, b, c]),
            _ => None,
        }
    };
    let [year, month, day] = fields(date, '-', [4, 2, 2])?;
    let [hour, minute, second] = fields(time, ':', [2, 2, 2])?;
    let valid = !fraction.is_empty()
        && fraction.bytes().all(|byte| byte.is_ascii_digit())
        && year >= 1970
        && (1..=12).contains(&month)
        && hour < 24
        && minute < 60
        && second < 60;
    let days = days_from_civil(year, month, day);
    // A day past the month's end lands in the next month.
    if !valid || civil_from_days(days) != (year, month, day) {
        return None;
    }
    u64::try_from(days * 86_400 + hour * 3_600 + minute * 60 + second).ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn months_are_calendar_months_in_utc() {
        for (unix, month) in [
            (0, "1970-01"),
            (951_782_399, "2000-02"),
            (951_782_400, "2000-02"), // 2000-02-29, a leap day
            (951_868_800, "2000-03"),
            (1_798_761_599, "2026-12"),
            (1_798_761_600, "2027-01"),
            (1_791_158_400, "2026-10"),
        ] {
            assert_eq!(month_key(unix), month, "{unix}");
        }
    }

    #[test]
    fn utc_times_are_read_and_others_refused() {
        for (text, unix) in [
            ("1970-01-01T00:00:00Z", 0),
            ("2026-10-18T19:00:00Z", 1_792_350_000),
            ("2026-10-18T19:00:00.750Z", 1_792_350_000),
            ("2026-10-18T19:00:00+00:00", 1_792_350_000),
            ("2024-02-29T23:59:59Z", 1_709_251_199),
            ("2030-01-01T03:00:00Z", 1_893_466_800),
        ] {
            assert_eq!(parse_utc(text), Some(unix), "{text}");
        }
        for text in [
            "2026-10-18T19:00:00",
            "2026-10-18T19:00:00+02:00",
            "2026-10-18 19:00:00Z",
            "2026-10-18T19:00Z",
            "2026-02-29T00:00:00Z",
            "2026-13-01T00:00:00Z",
            "2026-10-18T24:00:00Z",
            "2026-10-18T19:00:00.Z",
            "1969-12-31T23:59:59Z",
            "+2026-10-18T19:00:00Z",
        ] {
            assert_eq!(parse_utc(text), None, "{text}");
        }
    }
}
