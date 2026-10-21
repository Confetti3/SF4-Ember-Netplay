-- Webhooks lease each subscription's oldest pending event (spec 20.4).
CREATE INDEX delivery_subscription_order ON delivery_outbox (subscription_id, event_seq) WHERE state = 'pending';
