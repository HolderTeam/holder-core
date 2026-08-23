Our different concepts

| Object       | Purpose                                    | Structure              | Owns bytes?           |
| ------------ | ------------------------------------------ | ---------------------- | --------------------- |
| **Card**     | Human thought/information                  | Freeform               | No                    |
| **Resource** | A thing Holder understands                 | Ruthlessly structured  | No                    |
| **Asset**    | A digital artefact Holder knows about      | Fixed metadata         | Yes / represents them |
| **Location** | Somewhere Holder can store/retrieve assets | Provider configuration | No                    |

The important architectural point is that these should be **composable rather than competing representations**. A Resource can reference Assets; Cards can connect to Resources and Assets; an Asset lives at a Location. You shouldn't have to decide whether a PDF is “a Card or a Resource”.

### 1. Location first

I'd actually implement this from the bottom upward. Until Locations exist, Assets don't have a proper home.

A Location needs a small common interface regardless of backend:

```text
Location
    id
    name
    type
    configuration
    capabilities
    enabled
```

With initial types something like:

```text
local
webdav
s3
dropbox
nextcloud
```

The important API is much smaller than the configuration:

```cpp
put(asset)
get(asset)
exists(asset)
delete(asset)
```

Perhaps `list()` eventually, although I'd resist making Holder's model depend upon being able to enumerate a Location.

Credentials **do not belong in the Location record**. It holds a secret reference; `PlatformKeyring`/your secret infrastructure supplies the actual password/token.

### 2. Asset second

An Asset should have a Holder identity independent of its filename or physical location.

Something roughly like:

```text
Asset
    id
    filename
    media_type
    size
    hash
    created_at
    modified_at

    location_id
    object_key
```

That last distinction is useful:

```text
Location:
    Home NAS
    webdav://.../holder/

Asset:
    manuals/boiler/worcester.pdf
```

rather than storing a giant absolute URL on the Asset.

And I'd make **hash mandatory**. It gives you integrity checking, duplicate detection and eventually much better sync behaviour essentially for free.

### 3. Don't make Location synonymous with one copy

This is the one bit I'd design for now even if you don't implement it immediately.

Eventually the same Asset may be:

```text
Boiler Manual.pdf
        │
        ├── Home NAS
        ├── Razer Blade
        └── Android cache
```

So conceptually the cleaner model is actually:

```text
Asset
   │
   └── AssetPlacement
           ├── location_id
           ├── object_key
           └── state
```

rather than putting `location_id` directly on Asset.

You could initially enforce **one permanent placement per Asset**, but having the intermediate concept prevents a painful schema rethink once caching/replication arrives.

### 4. Resource third

Resources then sit _above_ Assets.

Kind of like a Dublin-Core Resource or a Platform Form (the class not the instance of it).

For example:

```text
Person
Book
Document
Website
Device
Account
Warranty
```

Each Resource has common identity:

```text
Resource
    id
    type
    name
    created_at
    modified_at
```

but the payload is defined by its type:

```text
Book
    title
    authors
    isbn
    publisher
    publication_date
    edition
```

This is where **“ruthlessly structured” needs teeth**. If `isbn` is an ISBN, Holder knows it's an ISBN. Dates are dates. URLs are URLs. People aren't arbitrary bags of strings.

We want to be able to represent Dublin-Core like Resources, but not confuse the user who just wants to write a shopping list, the Dublin Core vocabulary, the DCMI Metadata Terms, can be in there as optional properties.

### 5. Relationships tie the world together

Then you get the genuinely interesting model:

```text
                    Card
                     │
               "About / uses"
                     │
                  Resource
                 "My boiler"
                  /      \
                 /        \
          Asset             Asset
       Manual.pdf       Warranty.pdf
           │                 │
       Placement         Placement
           │                 │
         NAS               NAS
```

Your existing connection machinery can potentially carry quite a lot of this, although I'd distinguish **semantic connections** from **structural ownership/reference relationships**. `Parent of`, `related to`, etc. are user knowledge relationships; `AssetPlacement → Location` is application data and shouldn't masquerade as one.

### 6. First usable vertical slice

1. **Locations:** `LocalDirectory` + `WebDAV`.
2. **Assets:** import a file, hash it, record metadata, send it to the selected Location.
3. **Cards:** attach/link an Asset to a Card.
4. **Other device:** sync the metadata, see that the Asset exists, and fetch it if its Location is accessible.
5. **Resources:** introduce them once the Asset/Location foundation works.

That gets you very quickly to the behaviour that actually matters: **“I have a PDF associated with this card; Holder knows where it is; another Holder client knows it exists even if it can't currently retrieve it.”**

Then Resources can arrive without being tangled up with the storage problem.

And there's a principle emerging here that I'd probably write at the top of the design document:

> **Cards describe. Resources model. Assets contain. Locations store.**

That's concise enough that when some horrible edge case appears six months from now, we can ask which verb it belongs to and have a decent chance of putting it in the right subsystem.
