//! Both sides of a control handshake whose proof is not a `RoomProof`: a public
//! room's. The private handshake in the parent module is unchanged.
use super::*;

/// How long a host waits for a refused joiner to acknowledge `PublicRefused`
/// before it closes anyway. The refusal stands whether or not the frame lands.
const REFUSAL_FLUSH: Duration = Duration::from_secs(2);

/// `check` sees the proof and the connection before `Accepted` is sent; an
/// error refuses the peer exactly as `accept_control` does, and the account and
/// ticket it returns are kept on the channel.
///
/// A proof that does not parse, and a connection on another ALPN, end in a
/// silent close. A well-formed proof that `check` refuses is answered with
/// `PublicRefused` first, which carries no reason.
pub(crate) async fn accept_control_with<P, F>(
    connection: Connection,
    check: F,
) -> io::Result<ControlChannel>
where
    P: serde::de::DeserializeOwned,
    F: FnOnce(P, &Connection) -> io::Result<Option<(EmberId, RoomTicket)>>,
{
    let mut pending = PendingConnection(Some(connection.clone()));
    let result = timeout(HANDSHAKE_TIMEOUT, async {
        if connection.alpn() != CONTROL_ALPN {
            return Err(failed());
        }
        let (mut send, mut recv) = connection.accept_bi().await.map_err(|_| failed())?;
        let proof: P = read_handshake(&mut recv).await?;
        let admitted = match check(proof, &connection) {
            Ok(admitted) => admitted,
            Err(error) => {
                send_refusal(&mut send).await;
                return Err(error);
            }
        };
        send_handshake(&mut send, &Accepted { version: VERSION }).await?;
        let mut channel = control_channel(connection, (send, recv));
        if let Some((account, ticket)) = admitted {
            channel.account = Some(account);
            channel.ticket = Some(ticket);
        }
        Ok(channel)
    })
    .await
    .map_err(|_| failed())?;
    if result.is_ok() {
        pending.0 = None;
    }
    result
}

/// Sends `PublicRefused` and waits, briefly, for the joiner to have it, so the
/// close that follows cannot discard the frame.
async fn send_refusal(send: &mut SendStream) {
    if send_handshake(send, &PublicRefused { version: VERSION })
        .await
        .is_ok()
        && send.finish().is_ok()
    {
        let _ = timeout(REFUSAL_FLUSH, send.stopped()).await;
    }
}

/// `connect_control_on_proof` for a public proof: the host answers with
/// `Accepted`, or with `PublicRefused`, which is `admission_refused` and not a
/// failure of the connection. Running out of time is `handshake_timeout`.
pub(crate) async fn connect_public_on_proof<P: Serialize + Sync>(
    connection: Connection,
    proof: &P,
    expected: Option<EndpointId>,
) -> io::Result<ControlChannel> {
    let mut pending = PendingConnection(Some(connection.clone()));
    let result = timeout(HANDSHAKE_TIMEOUT, async {
        if connection.alpn() != CONTROL_ALPN
            || expected.is_some_and(|expected| connection.remote_id() != expected)
        {
            return Err(failed());
        }
        let (mut send, mut recv) = connection.open_bi().await.map_err(|_| failed())?;
        send_handshake(&mut send, proof).await?;
        match read_handshake(&mut recv).await? {
            PublicReply::Accepted(Accepted { version }) if version == VERSION => {
                Ok(control_channel(connection, (send, recv)))
            }
            PublicReply::Refused(PublicRefused { version }) if version == VERSION => {
                Err(admission_refused())
            }
            _ => Err(failed()),
        }
    })
    .await
    .map_err(|_| timed_out())?;
    if result.is_ok() {
        pending.0 = None;
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_refusal_and_the_acceptance_are_not_readable_as_each_other() {
        let accepted = serde_json::to_vec(&Accepted { version: VERSION }).unwrap();
        let refused = serde_json::to_vec(&PublicRefused { version: VERSION }).unwrap();
        assert_ne!(accepted, refused);
        assert!(serde_json::from_slice::<Accepted>(&refused).is_err());
        assert!(serde_json::from_slice::<PublicRefused>(&accepted).is_err());
    }

    #[test]
    fn a_refusal_round_trips_and_carries_nothing_else() {
        let bytes = serde_json::to_vec(&PublicRefused { version: VERSION }).unwrap();
        assert_eq!(
            serde_json::from_slice::<PublicRefused>(&bytes).unwrap().version,
            VERSION
        );
        assert!(matches!(
            serde_json::from_slice::<PublicReply>(&bytes),
            Ok(PublicReply::Refused(PublicRefused { version })) if version == VERSION
        ));
        let accepted = serde_json::to_vec(&Accepted { version: VERSION }).unwrap();
        assert!(matches!(
            serde_json::from_slice::<PublicReply>(&accepted),
            Ok(PublicReply::Accepted(Accepted { version })) if version == VERSION
        ));
        // A reason, or any other field, is not part of the frame.
        assert!(
            serde_json::from_slice::<PublicReply>(
                format!(r#"{{"refused":{VERSION},"reason":"banned"}}"#).as_bytes()
            )
            .is_err()
        );
        assert!(serde_json::from_slice::<PublicReply>(b"{}").is_err());
    }

    #[test]
    fn the_handshake_errors_are_told_apart() {
        assert!(is_admission_refused(&admission_refused()));
        assert!(is_handshake_timeout(&timed_out()));
        for error in [failed(), timed_out(), admission_refused()] {
            assert!(!is_host_unreachable(&error));
        }
        assert!(!is_admission_refused(&failed()));
        assert!(!is_admission_refused(&timed_out()));
        assert!(!is_handshake_timeout(&failed()));
        assert!(!is_handshake_timeout(&admission_refused()));
    }
}
