//! Glicko-2 (Glickman, "Example of the Glicko-2 system", 2013) with a rating
//! period of one game, the way Lichess runs it: every game updates both
//! players at once. Ratings are on the Glicko scale (1500 and 350 at the
//! start); the arithmetic happens on the Glicko-2 scale inside.
use std::f64::consts::PI;

/// Glicko-2 scale factor between the two scales.
const SCALE: f64 = 173.7178;
/// The system constant: how much volatility may change per game. Glickman
/// suggests 0.3 to 1.2.
const TAU: f64 = 0.5;
/// Convergence tolerance of the volatility iteration.
const EPSILON: f64 = 0.000_001;

/// A player's rating, its deviation (how unsure it is) and its volatility
/// (how erratic the player's results are), on the Glicko scale.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Rating {
    pub rating: f64,
    pub deviation: f64,
    pub volatility: f64,
}

impl Rating {
    /// Where every player starts, and the most unsure a rating can be.
    pub const NEW: Self = Self {
        rating: 1500.0,
        deviation: 350.0,
        volatility: 0.06,
    };

    /// This rating after `periods` rating periods without a game: the
    /// deviation grows by the volatility each period, never past a new
    /// player's.
    pub fn idle(self, periods: f64) -> Self {
        let phi = self.deviation / SCALE;
        let grown = (phi * phi + periods.max(0.0) * self.volatility * self.volatility).sqrt();
        Self {
            deviation: (grown * SCALE).min(Self::NEW.deviation),
            ..self
        }
    }

    /// This rating after one game against `opponent`: `score` is 1 for a
    /// win and 0 for a loss. `weight`, above 0 and at most 1, is how much of
    /// a game it counts as: a fraction of a rating period, whose information
    /// and whose volatility change both scale with it, so a game of weight
    /// 0.1 moves the rating about a tenth as far as a full one.
    pub fn after(self, opponent: Self, score: f64, weight: f64) -> Self {
        let weight = weight.clamp(f64::MIN_POSITIVE, 1.0);
        let (mu, phi, sigma) = (
            (self.rating - 1500.0) / SCALE,
            self.deviation / SCALE,
            self.volatility,
        );
        let (mu_j, phi_j) = (
            (opponent.rating - 1500.0) / SCALE,
            opponent.deviation / SCALE,
        );
        let g = 1.0 / (1.0 + 3.0 * phi_j * phi_j / (PI * PI)).sqrt();
        let expected = 1.0 / (1.0 + (-g * (mu - mu_j)).exp());
        let v = 1.0 / (weight * g * g * expected * (1.0 - expected));
        let delta = v * weight * g * (score - expected);
        let sigma = sigma + weight * (volatility(phi, sigma, v, delta) - sigma);
        let phi_star = (phi * phi + weight * sigma * sigma).sqrt();
        let phi = 1.0 / (1.0 / (phi_star * phi_star) + 1.0 / v).sqrt();
        let mu = mu + phi * phi * weight * g * (score - expected);
        Self {
            rating: mu * SCALE + 1500.0,
            deviation: (phi * SCALE).min(Self::NEW.deviation),
            volatility: sigma,
        }
    }
}

/// The new volatility, by the Illinois method (step 5 of the paper).
fn volatility(phi: f64, sigma: f64, v: f64, delta: f64) -> f64 {
    let a = (sigma * sigma).ln();
    let f = |x: f64| {
        let ex = x.exp();
        let d = phi * phi + v + ex;
        ex * (delta * delta - d) / (2.0 * d * d) - (x - a) / (TAU * TAU)
    };
    let mut big_a = a;
    let mut big_b = if delta * delta > phi * phi + v {
        (delta * delta - phi * phi - v).ln()
    } else {
        let mut k = 1.0;
        while f(a - k * TAU) < 0.0 {
            k += 1.0;
        }
        a - k * TAU
    };
    let (mut f_a, mut f_b) = (f(big_a), f(big_b));
    while (big_b - big_a).abs() > EPSILON {
        let big_c = big_a + (big_a - big_b) * f_a / (f_b - f_a);
        let f_c = f(big_c);
        if f_c * f_b <= 0.0 {
            (big_a, f_a) = (big_b, f_b);
        } else {
            f_a /= 2.0;
        }
        (big_b, f_b) = (big_c, f_c);
    }
    (big_a / 2.0).exp()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn close(a: f64, b: f64, within: f64) -> bool {
        (a - b).abs() <= within
    }

    /// The paper's worked example is one period with three games; with one
    /// game per period its first step (a 1500/200 player beating a 1400/30
    /// one) has a known result too, and the volatility step is checked
    /// against the paper's own numbers through the same function.
    #[test]
    fn the_papers_volatility_step() {
        // Step 3 and 4 of the example: v = 1.7785, delta = -0.4834 for
        // phi = 1.1513, sigma = 0.06; step 5 gives 0.05999.
        let sigma = volatility(200.0 / SCALE, 0.06, 1.7785, -0.4834);
        assert!(close(sigma, 0.05999, 0.000_01), "{sigma}");
    }

    #[test]
    fn a_win_raises_a_loss_lowers_and_both_grow_surer() {
        let player = Rating {
            rating: 1500.0,
            deviation: 200.0,
            volatility: 0.06,
        };
        let opponent = Rating {
            rating: 1400.0,
            deviation: 30.0,
            volatility: 0.06,
        };
        let won = player.after(opponent, 1.0, 1.0);
        let lost = player.after(opponent, 0.0, 1.0);
        assert!(close(won.rating, 1563.6, 0.5), "{won:?}");
        assert!(close(won.deviation, 175.4, 0.5), "{won:?}");
        assert!(lost.rating < 1500.0 && lost.deviation < 200.0, "{lost:?}");
        // Beating a weaker player is worth less than losing to them costs.
        assert!(won.rating - 1500.0 < 1500.0 - lost.rating);
    }

    #[test]
    fn a_lighter_game_moves_a_rating_less() {
        let player = Rating {
            rating: 1800.0,
            deviation: 80.0,
            volatility: 0.06,
        };
        let opponent = Rating {
            rating: 1300.0,
            deviation: 80.0,
            volatility: 0.06,
        };
        let full = 1800.0 - player.after(opponent, 0.0, 1.0).rating;
        let light = 1800.0 - player.after(opponent, 0.0, 0.1).rating;
        assert!(light > 0.0 && light < full * 0.15, "{full} {light}");
        // A game that hardly counts leaves the rating as sure as it was.
        let barely = player.after(opponent, 0.0, 0.001);
        assert!((barely.deviation - 80.0).abs() < 0.5, "{barely:?}");
        assert!((barely.volatility - 0.06).abs() < 1e-4, "{barely:?}");
    }

    #[test]
    fn idle_time_makes_a_rating_less_sure_up_to_a_new_players() {
        let settled = Rating {
            rating: 1700.0,
            deviation: 80.0,
            volatility: 0.06,
        };
        let back = settled.idle(60.0);
        assert_eq!(back.rating, 1700.0);
        assert!(back.deviation > 80.0);
        assert_eq!(settled.idle(1e9).deviation, Rating::NEW.deviation);
        assert_eq!(settled.idle(-5.0), settled);
    }
}
