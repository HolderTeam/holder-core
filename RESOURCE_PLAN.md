# Resources, Assets and Storage Locations

## Status

This document is the implementation plan for replacing Holder's prototype `Resource` support with
the durable Resource, Asset and storage system.

It turns the concepts in `RESOURCE_IDEAS.md` into an implementation sequence. That earlier file
remains useful design history; this file is the decision-making plan.

The existing `Resource` class, SQLite table, repository and HTTP contract are disposable
prototypes. They do not constrain this design and will be replaced without a compatibility shim or
data migration. The new implementation starts with Git-backed project manifests as its source of
truth; SQLite is only a rebuildable projection.

The guiding rule is:

> **Cards describe. Resources model. Assets contain. Locations store.**

## Product model

### Projects are access boundaries

A Project is the unit of sharing, Git history and encryption. Resources, Assets, Placements and
Locations belong to exactly one Project. Holder does not create a global Resource catalogue in this
phase.

The same person, book or company may therefore be represented by separate Resources in `Home`,
`Family` and `GolfClub`. This prevents metadata from leaking across access boundaries. Import and
reconciliation between Projects can be added later.

### Resource

A Resource represents a thing Holder understands: a person, organisation, book, website, physical
object, document, photograph or anything else in the real or digital world.

```text
Resource
    resource_id
    project_id
    type
    label
    metadata
    created_at
    updated_at
```

- `type` is friendly free text with built-in suggestions such as `thing`, `document`, `image`,
  `person`, `organisation`, `book` and `website`. It is not a C++ subclass or a closed enum.
- `label` is required operational UI text. Imports may initialise it from a filename.
- `metadata` is a permissive map of property name to repeated text values:

  ```json
  {
    "title": ["A Brief History of Time"],
    "creator": ["Stephen Hawking"],
    "description": ["Paperback copy from the school library"],
    "identifier": ["ISBN 9780553380163"]
  }
  ```

- Empty metadata is valid and normal. A photograph of a boiler need not contain anything beyond a
  type and label.
- Known friendly property names map to Dublin Core terms during import and export. Holder stores and
  displays `description`, not `dcterms:description`.
- Unknown custom properties round-trip unchanged. Unknown external vocabulary terms retain their
  original qualified name or URI so specialist imports are not lossy.
- Values are strings in the initial model. The manifest version leaves room for richer values, such
  as Resource references or language-tagged text, without requiring them now.

The initial work provides a Dublin Core mapping registry and serialization tests, not RDF storage or
a general ontology system. Zotero, CSL JSON, BibTeX and other integrations will be adapters over the
Holder model rather than native Holder schemas.

### Asset

An Asset represents a digital artefact attached to a Resource.

```text
Asset
    asset_id
    resource_id
    original_filename
    media_type
    byte_size
    plaintext_sha256
    created_at
    updated_at
```

The ownership rules are strict:

- Every Asset belongs to exactly one Resource.
- A Resource may have zero or more Assets.
- A Card links to the Resource, not directly to storage.
- Technical Asset metadata is deterministic and populated even when Resource metadata is empty.
- The plaintext SHA-256 hash is mandatory and provides integrity checking and project-local
  duplicate detection.

If the same bytes are imported again within a Project, Holder reuses the existing live Resource and
Asset and only creates any missing Card relationship. A future explicit "import as a separate
Resource" action may override this; it is not part of the first milestone.

### Placement

A Placement records where one stored copy of an Asset is expected to exist.

```text
Placement
    placement_id
    asset_id
    location_id
    object_key
    encoding
    stored_byte_size
    stored_sha256
    created_at
```

- An Asset may eventually have several Placements for replication or caching.
- The first implementation creates one permanent Placement per new Asset.
- Availability is checked at runtime with the provider and is not treated as portable truth.
- `stored_sha256` covers the exact stored object and catches transport/storage corruption before
  decryption. `plaintext_sha256` verifies the recovered Asset after decryption.

Placement is an internal structural concept. The UI says, for example, "Stored in Family Assets."

### Storage Location

The user-facing term is **Storage Location**; the internal model is `Location`.

```text
Location
    location_id
    project_id
    name
    provider
    configuration
    created_at
    updated_at
```

A Location is Project-scoped because Projects are access boundaries. Its safe, portable declaration
is stored in the Project Git repository. Each Holder installation privately binds that declaration
to usable paths or credentials through `SecretStore`.

Initial providers:

1. `local_directory`
   - The private binding contains the absolute root path on that device.
   - This also supports mounted NAS, NFS, SMB and similar storage without Holder knowing the mount
     protocol.
2. `s3_compatible`
   - Shared configuration contains endpoint, region, bucket, object prefix and addressing style.
   - The private binding contains access key ID, secret access key and optional session token.
   - The provider targets AWS S3 and compatible services such as MinIO, Cloudflare R2 and Backblaze
     B2. It is not named or modelled as an AWS-only provider.

The first implementation does not enumerate a Location and does not create buckets. A provider only
needs `put`, `get`, `exists` and `remove`. Location deletion is refused while a live Placement refers
to it.

The preferred Location is device-specific for each Project. If none is configured, the first import
opens Location setup/selection rather than guessing.

## Durable representation

### Project Git layout

Use one versioned manifest per Resource, nesting its Assets and Placements:

```text
resources/ab/cd/<resource-id>.json
locations/ab/cd/<location-id>.json
```

Resource manifest shape:

```json
{
  "format_version": 1,
  "resource": {
    "resource_id": "...",
    "project_id": "...",
    "type": "image",
    "label": "Boiler photograph",
    "metadata": {},
    "created_at": 0,
    "updated_at": 0
  },
  "assets": [
    {
      "asset_id": "...",
      "original_filename": "IMG_4821.jpg",
      "media_type": "image/jpeg",
      "byte_size": 3842193,
      "plaintext_sha256": "...",
      "created_at": 0,
      "updated_at": 0,
      "placements": [
        {
          "placement_id": "...",
          "location_id": "...",
          "object_key": "...",
          "encoding": "holder_asset_v1",
          "stored_byte_size": 3842301,
          "stored_sha256": "...",
          "created_at": 0
        }
      ]
    }
  ]
}
```

Each manifest is canonical JSON with stable field ordering, UTF-8, a trailing newline and strict
identity/ownership validation. Sharded paths follow the existing Card convention and are derived
from the ID; a mismatched ID and path is a rebuild error.

For `encrypted_git` Projects, the complete manifest is encrypted with the existing Project key via
`encrypt_project_blob` before it is written. Plain Projects write readable JSON. Encryption safety
checks must include Resource and Location manifest paths before commit and push.

Separate manifests keep unrelated Git changes independent. Concurrent edits to the same Resource or
Location follow the existing Project Git conflict policy in the first version; there is no
field-by-field metadata merge.

### SQLite projection

Replace the prototype table with normalized rebuildable tables:

- `resources`
- `resource_metadata`, including value order for repeated properties
- `assets`
- `asset_placements`
- `storage_locations`

Repository methods validate Project ownership and return complete typed models. SQLite indexes cover
Project/type/updated ordering, Resource ownership, Asset plaintext hash and Placement Location.
Duplicate reuse is enforced by the import service, while the database remains capable of representing
intentional duplicate Assets in a later version.

The schema upgrade that introduces these tables deliberately discards rows from the prototype
`resources` table. There is no old-kind/URI conversion and no old endpoint compatibility layer.

### Rebuild and startup recovery

Extend `Rebuilder` so a Project rebuild:

1. Reads and decrypts Location and Resource manifests.
2. Validates versions, IDs, paths, Project ownership, hashes and internal references.
3. Clears the Project's Resource/Asset/Placement/Location projection in the existing transaction.
4. Inserts Locations, Resources, metadata, Assets and Placements.
5. Rebuilds Cards and their links against the reconstructed Resource IDs.
6. Reports counts and clear file-specific errors.

`StartupRecovery` must recognise a repository containing `resources/` or `locations/` even if it has
no Cards yet. Deleting SQLite and restarting Holder must recover all non-secret models from Git.
Location bindings then resolve independently from `SecretStore`; missing bindings produce
"configuration required," not corrupt data.

### Private Location bindings

Store each binding through the existing platform-independent `SecretStore` using a dedicated service
name and an account derived from `project_id + location_id`. The secret value is versioned JSON.

- Local binding: absolute directory path.
- S3 binding: credentials and any explicitly private endpoint override.
- Binding metadata exposes only a safe preview for UI listing.
- A separate per-Project `SecretStore` preference records the preferred Location on that device, so
  it also survives SQLite deletion and is not confused with a shared Project default.
- Removing SQLite does not remove bindings.
- Removing a Project or Location does not silently delete credentials; explicit unbind/removal does.

The Project recovery token continues to recover the Project encryption key. It does not contain S3
credentials. A second family member imports the Project key and supplies their own Location binding.

## Asset storage and encryption

### Storage provider boundary

Define a narrow provider interface around opaque staged files rather than provider-specific URLs:

```text
put(object_key, staged_file, stored_size, stored_sha256)
get(object_key, destination_file)
exists(object_key)
remove(object_key)
```

- Providers must not parse Resource metadata or perform encryption.
- Providers must not require object enumeration.
- Operations return typed errors for unavailable, authentication, permission, capacity, integrity
  and transient transport failures.
- `holder-core` owns the interface, import orchestration and `local_directory` implementation.
- Host applications inject network providers. `holder-daemon` supplies `s3_compatible`; Android can
  later supply a platform transport without making core depend on an AWS SDK.

Local writes use a temporary sibling object followed by atomic rename. An existing final key is
verified before reuse and never overwritten with different bytes.

### Streaming Asset envelope

The existing `HolderPriv1` envelope is suitable for small Git manifests but buffers and base64-encodes
the complete value. Assets require a separate binary, streaming format:

```text
HolderAsset1
    versioned header
    project key ID
    libsodium secretstream header
    authenticated encrypted chunks
    final chunk marker
```

Use libsodium XChaCha20-Poly1305 secretstream with the Project key. Bind the Project, Resource, Asset
and key IDs as authenticated metadata. While reading the source once, compute plaintext SHA-256 and
write encrypted chunks to a private temporary staging file. Compute stored size and SHA-256 over the
finished envelope.

- `encrypted_git` Projects always store `holder_asset_v1` encrypted objects.
- `plain` Projects store original bytes with `encoding = plain`, while still hashing and verifying
  them.
- Decryption rejects the wrong Project/key/Asset, changed chunks, truncation and missing final marker.
- No import or retrieval path loads the whole Asset into memory.
- Temporary plaintext and encrypted files use restrictive permissions and are removed after success
  or failure.

Use opaque provider keys, never filenames or plaintext hashes:

```text
<configured-prefix>/<project-id>/<asset-id>.holderasset
```

This prevents the storage provider from learning filenames or comparing plaintext content hashes.

### S3-compatible provider

Implement only the object operations required by Holder using the daemon's existing Boost Beast,
OpenSSL and TLS stack. Do not add the AWS C++ SDK.

- Implement AWS Signature Version 4 with official test vectors.
- Support configurable HTTPS endpoint, region, bucket, prefix and path-style/virtual-host addressing.
- Require verified HTTPS except for an explicit localhost development mode used by MinIO tests.
- Use known staged-file length and hash for signed single-request PUTs.
- Retry idempotent operations on bounded transient failures with backoff.
- Verify successful PUT with `HEAD` before committing a Placement.
- Support objects up to S3's single-PUT limit in the first version; multipart upload is later work.
- A Location test uploads, verifies and removes a small random probe beneath its configured prefix.

## Import and retrieval workflows

### Background import

Dropping a file creates one background job per file. The desktop leaves the original untouched and
sends the local source URI, selected Project/Card and Location to the daemon. The daemon validates
that it is a readable regular file and never persists the source path into Git metadata.

Job states are `queued`, `staging`, `storing`, `committing`, `completed` and `failed`. Jobs and staging
paths are operational state, not Project truth; a daemon restart marks interrupted work failed and
cleans stale staging files.

Import ordering:

1. Inspect filename, MIME type and size.
2. Stream/hash and, for an encrypted Project, encrypt into staging.
3. Check for an existing live Asset with the same plaintext hash.
4. On a duplicate, discard staging, reuse its Resource/Asset and add only the missing Card link.
5. Otherwise create Resource, Asset and Placement IDs and store the staged object.
6. Verify the stored object through the provider.
7. Write and stage the Resource manifest and rewritten Card front matter.
8. Run the encrypted-Project staged-path safety check.
9. Update the SQLite projection in one transaction.
10. Commit all Project files once and report completion.

The new Resource type defaults from MIME information: images become `image`, PDF/text-like files
become `document`, and unknown files become `thing`. The initial label is the filename. Resource
metadata remains empty. The Card link uses `to_type = resource`, `kind = attachment` and an optional
filename label.

Resource/Card Git changes must form one logical mutation. Introduce a small Project mutation/batch
facility, or equivalent deferred-commit support, rather than allowing `CardStore` and `ResourceStore`
to create separate commits for one import.

Failure rules:

- Before storage succeeds: remove staging and create no durable model.
- After storage but before Git commit: remove the newly uploaded object when safe; otherwise log an
  unreferenced object for later garbage collection.
- After Git commit: Git is authoritative. If projection fails, rebuild the Project from manifests.
- Never create a Card link until the target Resource manifest is durable.
- Retrying the same job/object key is idempotent.

The initial daemon accepts local file URIs because desktop and daemon share a machine. A future
streaming upload endpoint will support sandboxed or remote clients without changing the core import
service.

### Retrieval and opening

To open an Asset:

1. Resolve its Placement and device binding.
2. Download/copy the opaque object into the daemon cache.
3. Verify stored SHA-256.
4. Decrypt if necessary while verifying final authentication and plaintext SHA-256.
5. Expose the recovered bytes through an authenticated streaming response with the original filename
   and media type.
6. Let the desktop open or save the result using the platform launcher/file dialog.

Recovered plaintext uses a private bounded cache and an age/size cleanup policy. It is never placed in
the Project Git repository or Location.

Deleting a Resource removes its manifest and Card links but does not immediately delete remote Asset
objects. Git history remains recoverable and destructive storage garbage collection is an explicit
later feature. This deliberately prefers recoverability over immediately reclaiming inexpensive
storage.

## Daemon and desktop surface

### Daemon API

Replace the prototype Resource endpoints with a complete contract:

- Resource list/get/update/delete, including metadata.
- Asset list and authenticated content retrieval.
- Location list/create/update/delete/test/bind/unbind.
- Start import and poll import-job status.

All routes enforce Project ownership and return typed errors for missing Location bindings, unavailable
storage, failed integrity checks and missing Project keys. Update the OpenAPI document and `holderctl`
with equivalent Location setup/test, Resource inspection and file import commands so the feature can
be exercised without GTK.

Once the C++ service contract is stable, expose Resource, Asset and Location read/mutation operations
through holder-core's JSON-returning C API. This keeps Android on the same domain and manifest model.
Network-provider injection across JNI and Android UI work remain deferred; the C API must not grow a
second Resource representation.

### Holder desktop

- Add a Project-scoped **Storage Locations** section to the Resources tool.
- Configure Local Directory and S3-compatible Locations with progressive disclosure.
- Store credentials only through daemon binding endpoints; never retain them in GSettings.
- Allow a per-device preferred Location for each Project.
- Accept one or more dropped files on the current Card without inserting their paths into editor text.
- Show background progress through the existing activity/status surfaces and refresh the Resource
  view on completion.
- Display Resources first and their Assets beneath them; Placements appear only as availability and
  "stored in" information.
- Open/download Assets through the daemon rather than accessing configured storage directly.
- Keep the default metadata editor simple: label, type and description. Put repeatable/custom
  metadata behind **Additional Details**.

Metadata extraction and LLM assistance are later additions. They must produce reviewable proposed
changes (nudges), never silently mutate Resource metadata or upload content to a cloud model.

## Implementation sequence

### 1. Replace the prototype domain and persistence

- Introduce the new models, JSON serializers, path helpers, metadata mapping registry and normalized
  repositories.
- Replace schema version 3 Resource storage with the new tables, deliberately dropping prototype
  Resource rows.
- Add Git-backed `ResourceStore` and `LocationStore`, encrypted/plain manifest support and rebuild.
- Retain `to_type = resource` as the Card relationship contract.

### 2. Add Asset crypto and local storage

- Implement the streaming Asset envelope and staged-file utilities.
- Implement provider interfaces, typed failures and Local Directory.
- Add binding storage and core import/retrieval services with fake-provider tests.
- Exercise the entire flow through `holderctl` before adding network or UI complexity.

### 3. Add S3-compatible family storage

- Add the daemon S3 transport/provider and binding/API surface.
- Add MinIO integration tests and a manual AWS-compatible smoke-test recipe.
- Verify two Holder installations can use separate credentials for one Project Location.

### 4. Add desktop drag-and-drop and management UI

- Add Storage Location configuration and testing.
- Add multi-file Card drop, import progress, Resource/Asset presentation and opening.
- Keep all storage, hashing, encryption and Git work in core/daemon.

### 5. Prove rebuild and family use

- Create an encrypted shared Project and S3-compatible Location.
- Import images and PDFs from one installation.
- Push/pull Project metadata through Git.
- Import the Project recovery key and bind the Location on a second installation.
- Retrieve and verify the Assets there.
- Delete SQLite on both installations and prove manifests plus keyring bindings reconstruct the same
  usable state.

## Test and acceptance plan

### Core tests

- Resource/Location manifest canonical round-trip, empty metadata, repeated values, Unicode, unknown
  custom properties and known Dublin Core mappings.
- Ownership, path, version, duplicate ID and malformed-reference rejection.
- Encrypted and plain manifest write/read and encryption safety checks.
- Streaming Asset round-trip over empty, tiny, multi-chunk and large inputs.
- Wrong key/identity, modified header/chunk, truncation, missing final marker and hash mismatch.
- Local provider atomic put/get/exists/remove and filesystem failure injection.
- Import duplicate reuse, Card-link idempotency and failure cleanup at every ordering boundary.
- Full Project rebuild after deleting SQLite, including Resource-targeting Card links.

### Daemon tests

- Route authentication, validation, Project isolation, missing binding/key and job-state behavior.
- Location credentials never appear in API responses, logs, Git files or SQLite.
- S3 Signature V4 official vectors and request construction.
- MinIO integration for put/head/get/delete, wrong credentials, unavailable endpoint, retry and
  tampered object.
- Retrieval streaming, filename/content type and cache cleanup.

### Desktop tests

- Drop routing for one/many files, unsupported/non-file data and no selected Card/Location.
- Location setup, secret redaction, test success/failure and preferred-Location behavior.
- Progress, retry, duplicate completion, Resource refresh and Asset opening.

### Milestone acceptance

The first family-usable milestone is complete when:

> A user can drag a PDF or image onto a Card, Holder creates and links a sparse Resource, encrypts and
> stores its Asset in Local Directory or S3-compatible storage, syncs all non-secret metadata through
> Git, and another authorised Holder installation can rebuild, retrieve, decrypt and open it.

## Explicitly deferred

- WebDAV and cloud-provider-specific SDKs.
- Multipart S3 uploads and automatic bucket creation.
- Asset replication, automatic caching Placements and cross-Location repair.
- Destructive orphan-object garbage collection.
- Global Resources or Locations across Project access boundaries.
- Resource schemas, C++ subclasses, RDF storage and mandatory metadata.
- Automatic EXIF/PDF/OCR/LLM metadata mutation.
- Zotero/BibTeX/CSL import/export implementation; only the metadata shape and mapping extension points
  are established here.
- Remote-client upload streaming and Android UI integration.
