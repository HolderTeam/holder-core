# Holder Milestones — Initial Specification

A **Milestone** represents a user-defined point or span of time that is significant to a Card.

A Card may have **zero or more Milestones**. Each Milestone belongs to exactly one Card.

## Model

```text
Milestone
────────────────────────
milestone_id
card_id

start
end             nullable
all_day

kind
description

created_at
updated_at
```

### Fields

**`milestone_id`**
Stable UUID identifying the Milestone. Milestones have their own identity, particularly for synchronisation, modification and deletion.

**`card_id`**
UUID of the Card that owns the Milestone.

**`start`**
Required date/time representing the beginning of the Milestone.

**`end`**
Optional date/time representing the end of the Milestone.

If `end` is null, the Milestone represents a single date or point in time. If present, the Milestone represents a span.

**`all_day`**
Boolean indicating whether time-of-day is significant.

When `true`, Holder presents the Milestone as a calendar date or date range without a time.

When `false`, the time component of `start` and `end` is significant.

**`kind`**
Optional short human-readable classifier.

Examples:

```text
Deadline
Appointment
Event
Exam
Birthday
Expiry
Renewal
Service
MOT
```

`kind` is vocabulary, not schema. Holder may suggest common kinds, but arbitrary user-defined values are valid. The core Milestone implementation does not need to understand their semantics.

**`description`**
Optional free-form text describing the significance of the Milestone.

**`created_at`**
System timestamp recording when the Milestone itself was created.

**`modified_at`**
System timestamp recording when the Milestone itself was last modified.

`created_at` and `updated_at` are bookkeeping metadata and are distinct from the time represented by `start` and `end`.

## Examples

```text
Card: My Car

Milestone
  start:       2026-09-18
  end:         null
  all_day:     true
  kind:        Renewal
  description: Car insurance renewal

Milestone
  start:       2026-09-25 09:30
  end:         2026-09-25 11:00
  all_day:     false
  kind:        Service
  description: Annual service at garage
```

## Calendar

Milestones are the user-defined temporal information attached to Cards.

The Calendar is a **view over Holder data**, not part of the Milestone model itself. It can combine:

```text
Card.created_at
Card.modified_at
Milestone.start / Milestone.end
```

This allows a date in the Calendar to show Cards created or modified on that date alongside user-defined Milestones.

## Initial UI

The basic Card interface should expose Milestones simply:

```text
Milestones

18 Sep 2026       Insurance renewal
25 Sep 2026 09:30 Annual service
03 May 2027       MOT due

+ Add milestone
```

Creating or editing a Milestone exposes:

```text
Date        [ 25 Sep 2026 ]
Time        [ 09:30       ]
All day     [ off ]

Kind        [ Service     ]
Description [ Annual service at garage ]
```

When **All day** is enabled, time controls are hidden or disabled.

Have a button to add End, when End is enabled, Date becomes start.


## Design Principle

A Milestone is deliberately a small primitive:

> **A Milestone says that a particular point or span of time matters to a Card.**

It does not inherently represent a task, appointment, deadline, reminder or event. Those meanings belong to the user and may be expressed through `kind` and `description`.

More specialised behaviour—recurrence, reminders, completion state, attendees, alarms, and so on—should not be added until real Holder workflows demonstrate a need for it.
