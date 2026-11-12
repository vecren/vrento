export module vrento:texture.pool;
export import :texture.core;
import rstd;

export namespace vrento::texture
{
struct PoolStats {
    Count  capacity {}, available {}, writing {}, pending {}, leased {};
    Serial acquired {}, published {}, cancelled {}, busy {};
    bool   closed {}, failed {};
};
// Device creation uses its context owner; published leases may be released on other threads.
class TexturePool {
    struct Slot {
        TextureViewRef  view;
        bool            writing {};
        Count           readers {};
        SubmissionToken pending;
    };
    struct State {
        rstd::sync::Mutex<empty> mutex;
        Vec<Slot>                slots;
        Serial                   version {}, acquired {}, published {}, cancelled {}, busy {};
        bool                     closed {};
    };
    struct WritePin {
        Arc<State> owner;
        usize      slot;
        Serial     version;
        bool       active { true };
        WritePin(Arc<State> s, usize i, Serial v): owner(rstd::move(s)), slot(i), version(v) {}
        ~WritePin() {
            auto lock = owner->mutex.lock().unwrap();
            if (active) {
                auto& entry        = owner->slots[slot];
                auto  storage_lock = BackendAccess::Lock(entry.view.texture());
                entry.writing      = false;
                entry.view.texture().backend_state()->writer = 0;
                ++owner->cancelled;
            }
        }
    };
    struct ReadPin {
        Arc<State> owner;
        usize      slot;
        ReadPin(Arc<State> s, usize i): owner(rstd::move(s)), slot(i) {
            ++owner->slots[slot].readers;
        }
        ~ReadPin() {
            auto lock = owner->mutex.lock().unwrap();
            --owner->slots[slot].readers;
        }
    };
    Arc<State> state_;
    explicit TexturePool(Arc<State> state): state_(rstd::move(state)) {}

public:
    class Write {
        Arc<WritePin> pin_;
        explicit Write(Arc<WritePin> pin): pin_(rstd::move(pin)) {}
        friend class TexturePool;

    public:
        Write(const Write&)                    = delete;
        auto operator=(const Write&) -> Write& = delete;
        Write(Write&&)                         = default;
        auto operator=(Write&&) -> Write&      = default;
        auto view() const -> TextureViewRef {
            auto lock          = pin_->owner->mutex.lock().unwrap();
            auto result        = pin_->owner->slots[pin_->slot].view;
            result.permission_ = Permission::Write;
            result.version_    = pin_->version;
            result.pin_        = Some(Arc<dyn<rstd::any::Any>>::make(pin_.clone()));
            return result;
        }
        void cancel() { pin_.reset(); }
        bool valid() const {
            if (! pin_) return false;
            auto lock = pin_->owner->mutex.lock().unwrap();
            return pin_->active;
        }
    };
    TexturePool(const TexturePool&)                    = delete;
    auto operator=(const TexturePool&) -> TexturePool& = delete;
    TexturePool(TexturePool&&)                         = default;
    auto operator=(TexturePool&& other) -> TexturePool& {
        if (this != &other) {
            close();
            state_ = rstd::move(other.state_);
        }
        return *this;
    }
    ~TexturePool() { close(); }
    static auto create(mut_ref<dyn<TextureDevice>> device, TextureDesc desc, Count capacity = 3)
        -> Result<TexturePool, Error> {
        if (! capacity || capacity > 64) return Err(Error { ErrorKind::Invalid });
        auto checked = validate(desc);
        if (checked.is_err()) return Err(checked.unwrap_err());
        auto state = Arc<State>::make();
        for (Count i = 0; i < capacity; ++i) {
            auto allocated = device->CreateTexture(desc);
            if (allocated.is_err()) return Err(allocated.unwrap_err());
            ViewDesc definition { desc.dimension, { format_layout(desc.format).aspect } };
            auto     view = device->CreateView(allocated.unwrap(), definition);
            if (view.is_err()) return Err(view.unwrap_err());
            auto result                            = view.unwrap();
            result.texture().backend_state()->pool = 1;
            state->slots.push(Slot { rstd::move(result) });
        }
        return Ok(TexturePool(rstd::move(state)));
    }
    auto acquire() -> Result<Write, Error> {
        if (! state_) return Err(Error { ErrorKind::Closed });
        for (usize i {}; i < state_->slots.len(); ++i) {
            SubmissionToken pending;
            TextureViewRef  view;
            {
                auto lock = state_->mutex.lock().unwrap();
                if (state_->closed) return Err(Error { ErrorKind::Closed });
                const auto& slot = state_->slots[i];
                if (slot.writing || slot.readers) continue;
                view    = slot.view;
                pending = slot.pending;
            }
            auto checked = view.texture().check();
            if (checked.is_err()) return Err(checked.unwrap_err());
            auto completed = pending.poll();
            if (completed.is_err()) return Err(completed.unwrap_err());
            if (! completed.unwrap()) continue;
            auto lock = state_->mutex.lock().unwrap();
            if (state_->closed) return Err(Error { ErrorKind::Closed });
            auto& slot = state_->slots[i];
            if (slot.writing || slot.readers || ! (slot.pending == pending)) continue;
            auto  storage_lock = BackendAccess::Lock(slot.view.texture());
            auto& storage      = *BackendAccess::Storage(slot.view.texture());
            if (storage.recording || storage.published_reads || storage.prepared_reads) continue;
            if (state_->version == ~Serial {}) return Err(Error { ErrorKind::Invalid });
            auto pin       = Arc<WritePin>::make(state_.clone(), i, ++state_->version);
            slot.writing   = true;
            pending        = rstd::exchange(slot.pending, SubmissionToken {});
            storage.writer = pin->version;
            ++state_->acquired;
            return Ok(Write(rstd::move(pin)));
        }
        auto lock = state_->mutex.lock().unwrap();
        if (state_->closed) return Err(Error { ErrorKind::Closed });
        ++state_->busy;
        return Err(Error { ErrorKind::Busy });
    }
    auto publish(Write write, SubmissionToken submitted) -> Result<TextureViewRef, Error> {
        if (! state_) return Err(Error { ErrorKind::Closed });
        if (! write.valid() || &*write.pin_->owner != &*state_ || ! submitted.valid())
            return Err(Error { ErrorKind::Invalid });
        auto checked = state_->slots[write.pin_->slot].view.texture().check();
        if (checked.is_err()) return Err(checked.unwrap_err());
        auto lock = state_->mutex.lock().unwrap();
        if (state_->closed) return Err(Error { ErrorKind::Closed });
        auto& slot         = state_->slots[write.pin_->slot];
        auto  storage_lock = BackendAccess::Lock(slot.view.texture());
        auto& storage      = *BackendAccess::Storage(slot.view.texture());
        if (! (storage.last_write == submitted) || storage.recording)
            return Err(Error { ErrorKind::Invalid });
        slot.pending       = submitted;
        slot.writing       = false;
        storage.writer     = 0;
        storage.version    = write.pin_->version;
        write.pin_->active = false;
        auto result        = slot.view;
        result.permission_ = Permission::Read;
        result.version_    = storage.version;
        result.pin_        = Some(
            Arc<dyn<rstd::any::Any>>::make(Arc<ReadPin>::make(state_.clone(), write.pin_->slot)));
        ++state_->published;
        return Ok(rstd::move(result));
    }
    auto stats() const -> PoolStats {
        PoolStats result;
        if (! state_) {
            result.closed = true;
            return result;
        }
        Vec<Slot> slots;
        {
            auto lock        = state_->mutex.lock().unwrap();
            result.capacity  = Count(state_->slots.len().to_primitive());
            result.acquired  = state_->acquired;
            result.published = state_->published;
            result.cancelled = state_->cancelled;
            result.busy      = state_->busy;
            result.closed    = state_->closed;
            for (const auto& slot : state_->slots)
                slots.push(Slot { slot.view, slot.writing, slot.readers, slot.pending });
        }
        for (const auto& slot : slots) {
            auto checked = slot.view.texture().check();
            if (checked.is_err()) result.failed = true;
            result.writing += slot.writing;
            result.leased += slot.readers != 0;
            auto complete = slot.pending.poll();
            if (complete.is_err()) result.failed = true;
            bool pending = complete.is_err() || ! complete.unwrap();
            result.pending += pending;
            result.available +=
                ! slot.writing && ! slot.readers && ! pending && ! result.closed && checked.is_ok();
        }
        return result;
    }
    void close() {
        if (state_) {
            auto lock      = state_->mutex.lock().unwrap();
            state_->closed = true;
        }
    }
};
} // namespace vrento::texture
