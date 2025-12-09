// ============================================================================
// Örnek: Lock-free MPMC (Multiple Producer Multiple Consumer) Halka Buffer
// ============================================================================
// Bu implementasyon, lock-free (kilit kullanmayan) bir circular buffer sağlar.
// 
// TEMEL TASARIM:
// - Slot içinde veri tutulmuyor; tek bir büyük CPU char dizisi (data_cpu_) var
// - GPU tarafı simülasyonu için short dizisi (data_gpu_) var
// - Sabit chunk_size ile bu diziler chunk'lara bölünür
// - Her slot bir sequence counter tutar (seq) - bu lock-free senkronizasyon için kritik
// - Ek metadata: rfSignal (std::pair<int,double>) ve size (std::size_t)
// - Producer: claim -> chunk pointer al (cpu_ptr, gpu_ptr, rf, size_ptr) -> doldur -> commit
// - Consumer: claim -> pointer'ları al -> oku -> release
//
// LOCK-FREE ALGORİTMA:
// Sequence counter pattern kullanılıyor. Her slot'un bir "beklenen sıra numarası" var:
// - Boş slot: seq == pos (slot'un global pozisyonu)
// - Dolu slot: seq == pos + 1 (producer doldurdu, consumer bekliyor)
// - Yeniden boş: seq == pos + capacity (consumer okudu, producer tekrar kullanabilir)
//
// Bu sayede mutex/condition_variable olmadan thread-safe çalışma sağlanır.
//
// Build: g++ -std=c++20 -O2 -pthread main.cpp -o app
// Run:   ./app
// ============================================================================

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>
#include <algorithm>

// ============================================================================
// Lock-free Bounded MPMC Ring Buffer
// ============================================================================
// Bu sınıf, sequence counter pattern kullanarak lock-free çalışan bir circular
// buffer implementasyonu sağlar. Veri tek bir büyük char dizisinde tutulur ve
// sabit chunk_size ile bölünür. Her chunk bir slot'a karşılık gelir.
//
// ÖNEMLİ: Bu implementasyon wait-free değil, lock-free'dur. Yani bazı thread'ler
// diğerlerini bekleyebilir ama hiçbir thread mutex/condition_variable ile bloke
// olmaz - sadece spin/yield yapar.
// ============================================================================
class CircularBuffer {
public:
    // Producer/Consumer'ın claim ettiği slot bilgisini taşır
    struct Ticket {
        std::size_t pos;               // Slot'un global pozisyon numarası (ring buffer'da döngüsel)
        char* cpu_ptr;                 // CPU tarafı (char) chunk başlangıcı
        short* gpu_ptr;                // GPU tarafı (simüle) short chunk başlangıcı
        std::pair<int, double>* rf;    // Ek metadata: rfSignal
        std::size_t* size_ptr;         // Ek metadata: yazılan byte sayısı
    };

    // Constructor: buffer'ı belirtilen kapasite ve chunk boyutu ile başlatır
    CircularBuffer(std::size_t capacity_chunks, std::size_t chunk_size)
        : chunk_size_(chunk_size) {
        // Kapasiteyi 2'nin kuvveti yap (ör: 7 -> 8, 9 -> 16)
        // Bu sayede mod işlemi (pos % capacity) yerine bitwise AND (pos & mask) kullanabiliriz
        // Bitwise AND çok daha hızlıdır ve performans kritik bir noktadır
        capacity_ = 1;
        while (capacity_ < capacity_chunks) capacity_ <<= 1;  // power of two
        
        // Mask: capacity 8 ise mask = 7 (binary: 0111)
        // pos & mask işlemi pos % capacity ile aynı sonucu verir ama çok daha hızlı
        mask_ = capacity_ - 1;
        
        // Slot dizisini oluştur (her slot bir sequence counter tutar)
        // unique_ptr kullanıyoruz çünkü Slot içinde atomic var ve kopyalanamaz
        slots_ = std::make_unique<Slot[]>(capacity_);
        
        // Her slot'u başlangıç durumuna getir: seq = pos (boş durum)
        // memory_order_relaxed yeterli çünkü henüz thread'ler başlamadı
        for (std::size_t i = 0; i < capacity_; ++i) {
            slots_[i].seq.store(i, std::memory_order_relaxed);
        }
        
        // Büyük char dizisini oluştur: capacity * chunk_size byte
        // Her slot için chunk_size byte ayrılır
        data_cpu_.resize(capacity_ * chunk_size_);

        // "GPU" (simülasyon) için short dizisi: chunk_size / sizeof(short) kadar eleman
        shorts_per_chunk_ = chunk_size_ / sizeof(short);
        if (shorts_per_chunk_ == 0) shorts_per_chunk_ = 1;  // emniyet
        data_gpu_.resize(capacity_ * shorts_per_chunk_);

        // Metadata dizileri: rfSignal ve size
        meta_rf_signal_.resize(capacity_);
        meta_size_.resize(capacity_, 0);
    }

    // ========================================================================
    // Producer: Boş bir slot'u claim eder ve chunk pointer'ı döner
    // ========================================================================
    // Bu fonksiyon lock-free çalışır: mutex kullanmaz, sadece atomik işlemler
    // ve CAS (Compare-And-Swap) kullanır.
    //
    // AKIŞ:
    // 1. tail_ (son yazılan pozisyon) oku
    // 2. O pozisyondaki slot'un sequence'ını kontrol et
    // 3. Eğer slot boşsa (seq == pos), CAS ile tail'i ilerlet
    // 4. Başarılı olursa chunk pointer'ı döndür
    // 5. Değilse backoff yap ve tekrar dene
    //
    // ÖNEMLİ: Bu fonksiyon döndükten sonra mutlaka commit_producer() çağrılmalı!
    // ========================================================================
    Ticket claim_producer() {
        Backoff backoff;  // Contention durumunda bekleme stratejisi
        while (true) {
            // Shutdown kontrolü: eğer buffer kapatıldıysa çık
            if (shutdown_.load(std::memory_order_acquire)) {
                return Ticket{0, nullptr, nullptr, nullptr, nullptr};
            }

            // tail_: son yazılan pozisyon (atomik, birden fazla producer paylaşır)
            // memory_order_relaxed yeterli çünkü sadece okuma yapıyoruz
            std::size_t pos = tail_.load(std::memory_order_relaxed);
            
            // Ring buffer'da döngüsel indeks: pos & mask_ = pos % capacity_
            Slot& slot = slots_[pos & mask_];
            
            // Slot'un sequence değerini oku
            // memory_order_acquire: bu okumadan önceki tüm yazıları görürüz
            std::size_t seq = slot.seq.load(std::memory_order_acquire);
            
            // Sequence kontrolü: slot'un durumunu anlamak için
            // diff = seq - pos
            // diff == 0  -> slot boş, producer yazabilir
            // diff < 0   -> slot dolu, başka producer yazmış, bekle
            // diff > 0   -> beklenmeyen durum (race condition?), bekle
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            
            if (diff == 0) {  // Slot boş! Claim etmeyi dene
                // CAS (Compare-And-Swap): atomik olarak tail'i ilerlet
                // Eğer tail hala pos ise, pos+1 yap ve başarılı dön
                // Eğer başka bir producer tail'i değiştirdiyse, başarısız ol ve tekrar dene
                // memory_order_acq_rel: hem acquire hem release semantiği
                if (tail_.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
                    // Başarılı! Bu slot'u claim ettik
                    // CPU chunk başlangıcı
                    char* cpu_p = data_cpu_.data() + (pos & mask_) * chunk_size_;
                    // GPU chunk başlangıcı (short)
                    short* gpu_p = data_gpu_.data() + (pos & mask_) * shorts_per_chunk_;
                    // Metadata pointer'ları
                    auto* rf = &meta_rf_signal_[pos & mask_];
                    auto* sz = &meta_size_[pos & mask_];
                    return Ticket{pos, cpu_p, gpu_p, rf, sz};  // Producer bu pointer'lara yazabilir
                }
                // CAS başarısız oldu (başka producer önce davrandı), tekrar dene
            } else if (diff < 0) {  // Slot dolu, başka producer yazmış
                // Consumer henüz okumamış, bekle
                backoff();
            } else {  // diff > 0: Beklenmeyen durum (çok nadir)
                // Muhtemelen race condition, güvenli tarafta kal ve bekle
                backoff();
            }
        }
    }

    // ========================================================================
    // Producer: Chunk'ı doldurduktan sonra slot'u consumer'lara açık hale getirir
    // ========================================================================
    // Bu fonksiyon claim_producer()'dan sonra MUTLAKA çağrılmalı!
    //
    // Sequence'i pos+1 yaparak slot'u "dolu" olarak işaretleriz.
    // Consumer'lar seq == pos+1 olduğunu görünce bu slot'u okuyabilir.
    //
    // memory_order_release: Bu yazıdan önceki tüm yazılar (chunk içine yazılan
    // veriler) consumer'lar tarafından görülebilir hale gelir.
    // ========================================================================
    void commit_producer(const Ticket& t) {
        // Sequence'i pos+1 yap = "Bu slot dolu, consumer okuyabilir" sinyali
        slots_[t.pos & mask_].seq.store(t.pos + 1, std::memory_order_release);
    }

    // ========================================================================
    // Consumer: Dolu bir slot'u claim eder ve chunk pointer'ı döner
    // ========================================================================
    // Producer'ın tam tersi mantıkla çalışır:
    //
    // AKIŞ:
    // 1. head_ (son okunan pozisyon) oku
    // 2. O pozisyondaki slot'un sequence'ını kontrol et
    // 3. Eğer slot doluysa (seq == pos + 1), CAS ile head'i ilerlet
    // 4. Başarılı olursa chunk pointer'ı döndür
    // 5. Değilse backoff yap ve tekrar dene
    //
    // ÖNEMLİ: Bu fonksiyon döndükten sonra mutlaka release_consumer() çağrılmalı!
    // ========================================================================
    std::optional<Ticket> claim_consumer() {
        Backoff backoff;
        while (true) {
            // head_: son okunan pozisyon (atomik, birden fazla consumer paylaşır)
            std::size_t pos = head_.load(std::memory_order_relaxed);
            
            // Ring buffer'da döngüsel indeks
            Slot& slot = slots_[pos & mask_];
            
            // Slot'un sequence değerini oku
            std::size_t seq = slot.seq.load(std::memory_order_acquire);
            
            // Sequence kontrolü: slot'un durumunu anlamak için
            // diff = seq - (pos + 1)
            // diff == 0  -> slot dolu (seq == pos+1), consumer okuyabilir
            // diff < 0   -> slot boş (seq < pos+1), producer henüz yazmamış, bekle
            // diff > 0   -> beklenmeyen durum, bekle
            intptr_t diff =
                static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            
            if (diff == 0) {  // Slot dolu! Claim etmeyi dene
                // CAS ile head'i ilerlet
                if (head_.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
                    // Başarılı! Bu slot'u claim ettik
                    char* cpu_p = data_cpu_.data() + (pos & mask_) * chunk_size_;
                    short* gpu_p = data_gpu_.data() + (pos & mask_) * shorts_per_chunk_;
                    auto* rf = &meta_rf_signal_[pos & mask_];
                    auto* sz = &meta_size_[pos & mask_];
                    return Ticket{pos, cpu_p, gpu_p, rf, sz};  // Consumer bu pointer'lardan okuyabilir
                }
                // CAS başarısız oldu (başka consumer önce davrandı), tekrar dene
            } else if (diff < 0) {  // Slot boş, producer henüz yazmamış
                // Shutdown kontrolü: eğer buffer kapatıldıysa ve boşsa, çık
                if (shutdown_.load(std::memory_order_acquire)) return std::nullopt;
                backoff();  // Producer yazmasını bekle
            } else {  // diff > 0: Beklenmeyen durum
                backoff();
            }
        }
    }

    // ========================================================================
    // Consumer: Chunk'ı okuduktan sonra slot'u producer'lara geri verir
    // ========================================================================
    // Bu fonksiyon claim_consumer()'dan sonra MUTLAKA çağrılmalı!
    //
    // Sequence'i pos + capacity_ yaparak slot'u "boş" olarak işaretleriz.
    // Producer'lar seq == pos olduğunu görünce bu slot'a yazabilir.
    //
    // Neden pos + capacity_?
    // - Ring buffer döngüsel çalışır, pos değerleri sürekli artar
    // - pos + capacity_ yaparak, bir sonraki döngüde aynı slot'a geldiğimizde
    //   seq değerini doğru hesaplayabiliriz
    // - Örnek: capacity=8, pos=5 -> seq=13. Bir sonraki döngüde pos=13 geldiğinde
    //   seq kontrolü: 13 - 13 = 0 (boş) olur
    //
    // memory_order_release: Bu yazıdan önceki tüm okumalar (chunk'tan okunan
    // veriler) tamamlanmış olur.
    // ========================================================================
    void release_consumer(const Ticket& t) {
        // Sequence'i pos + capacity_ yap = "Bu slot boş, producer yazabilir" sinyali
        slots_[t.pos & mask_].seq.store(t.pos + capacity_,
                                        std::memory_order_release);
    }

    // ========================================================================
    // Stop: Buffer'ı kapatır, producer/consumer'lara çıkış sinyali gönderir
    // ========================================================================
    // Bu fonksiyon çağrıldığında:
    // - Producer'lar claim_producer()'da nullptr döner ve çıkar
    // - Consumer'lar claim_consumer()'da nullopt döner ve çıkar
    //
    // memory_order_release: Bu yazıdan önceki tüm işlemler tamamlanır
    // ========================================================================
    void stop() { shutdown_.store(true, std::memory_order_release); }

private:
    // ========================================================================
    // Slot: Her slot bir sequence counter tutar
    // ========================================================================
    // Sequence counter, lock-free senkronizasyonun kalbidir:
    // - seq == pos: Slot boş, producer yazabilir
    // - seq == pos + 1: Slot dolu, consumer okuyabilir
    // - seq == pos + capacity_: Slot boş (consumer okudu), producer tekrar yazabilir
    //
    // Copy/move constructor'lar delete edildi çünkü atomic kopyalanamaz.
    // Bu yüzden vector yerine unique_ptr kullanıyoruz.
    // ========================================================================
    struct Slot {
        std::atomic<std::size_t> seq{};  // Slot'un beklenen sıra numarası (sequence)
        Slot() = default;
        Slot(const Slot&) = delete;      // Atomic kopyalanamaz
        Slot& operator=(const Slot&) = delete;
        Slot(Slot&&) = delete;
        Slot& operator=(Slot&&) = delete;
    };

    // ========================================================================
    // Backoff: Contention (çakışma) durumunda bekleme stratejisi
    // ========================================================================
    // Lock-free algoritmalarda, eğer bir thread CAS başarısız olursa veya
    // beklediği durum henüz oluşmamışsa, sürekli döngüye girip CPU'yu
    // boşa harcamak yerine akıllıca beklemelidir.
    //
    // STRATEJİ:
    // 1. İlk 16 denemede: Exponential backoff (1, 2, 4, 8, ... spin)
    //    - Kısa süreli çakışmalarda hızlı tepki verir
    //    - atomic_signal_fence: Compiler'ın optimizasyonunu engeller, CPU pipeline'ı temizler
    // 2. 16 denemeden sonra: Thread yield (OS'a CPU'yu başka thread'e ver)
    //    - Uzun süreli beklemelerde CPU kaynaklarını boşa harcamaz
    //
    // Bu sayede hem düşük latency (kısa bekleme) hem de yüksek throughput
    // (uzun bekleme) sağlanır.
    // ========================================================================
    struct Backoff {
        void operator()() {
            if (count_ < 16) {
                // Exponential backoff: 1, 2, 4, 8, 16, ... spin
                for (int i = 0; i < (1 << count_); ++i) {
                    // Memory fence: Compiler optimizasyonlarını engeller
                    // CPU pipeline'ını temizler, cache coherence sağlar
                    std::atomic_signal_fence(std::memory_order_seq_cst);
                }
                ++count_;
            } else {
                // Uzun süre beklediysek, OS'a CPU'yu başka thread'e ver
                // Bu sayede CPU kaynaklarını boşa harcamayız
                std::this_thread::yield();
            }
        }
        int count_{0};  // Kaç kez backoff yaptık
    };

    // ========================================================================
    // Member Variables
    // ========================================================================
    std::size_t capacity_{0};      // Ring buffer kapasitesi (2'nin kuvveti)
    std::size_t mask_{0};          // Bitwise AND için mask (capacity - 1)
    std::size_t chunk_size_{0};    // Her chunk'ın byte cinsinden boyutu
    std::size_t shorts_per_chunk_{0};  // GPU short kapasitesi (chunk_size / sizeof(short))
    
    // Slot dizisi: Her slot bir sequence counter tutar
    // unique_ptr kullanıyoruz çünkü Slot içinde atomic var ve kopyalanamaz
    std::unique_ptr<Slot[]> slots_;
    
    // CPU tarafı: tüm chunk'lar char dizisinde tutulur
    std::vector<char> data_cpu_;
    // GPU tarafı (simülasyon): short dizisi
    std::vector<short> data_gpu_;
    // Ek metadata
    std::vector<std::pair<int, double>> meta_rf_signal_;
    std::vector<std::size_t> meta_size_;
    
    // head_: Consumer'ların okuduğu son pozisyon (atomik)
    // tail_: Producer'ların yazdığı son pozisyon (atomik)
    // alignas(64): False sharing'i önlemek için cache line (64 byte) hizalama
    // Birden fazla thread aynı cache line'ı paylaşırsa performans düşer
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    
    // Shutdown flag: Buffer'ın kapatıldığını gösterir
    // Producer/Consumer'lar bu flag'i kontrol ederek çıkış yapar
    alignas(64) std::atomic<bool> shutdown_{false};
};

// ============================================================================
// Thread-safe logging helper
// ============================================================================
// Çok thread'li ortamda cout'ların birbirine karışmaması için mutex ile korunur
// ============================================================================
namespace {
    std::mutex log_mutex;  // Global log mutex (sadece bu dosya içinde görünür)
    
    // Thread-safe log fonksiyonu: tüm çıktıyı atomik olarak yazar
    template<typename... Args>
    void safe_log(Args&&... args) {
        std::lock_guard<std::mutex> lock(log_mutex);
        (std::cout << ... << std::forward<Args>(args));
        std::cout << std::endl;  // Her log satırı sonunda newline
    }
}

// ============================================================================
// Main: Test programı
// ============================================================================
int main() {
    // Buffer parametreleri
    constexpr std::size_t buffer_capacity = 8;   // Ring buffer'da kaç chunk var
    constexpr std::size_t chunk_size = 64;       // Her chunk kaç byte (örn: 64 byte)
    constexpr int producer_count = 3;             // Kaç producer thread
    constexpr int consumer_count = 2;             // Kaç consumer thread
    constexpr int items_per_producer = 20;        // Her producer kaç item üretecek

    // Lock-free circular buffer oluştur
    CircularBuffer buffer(buffer_capacity, chunk_size);
    
    // İstatistikler için atomik sayaçlar
    std::atomic<int> produced_total{0};  // Toplam üretilen item sayısı
    std::atomic<int> consumed_total{0};   // Toplam tüketilen item sayısı

    // ========================================================================
    // Producer Lambda: Her producer thread bu fonksiyonu çalıştırır
    // ========================================================================
    auto producer = [&](int id) {
        // Rastgele sayı üretici (test için)
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(1, 1000);
        
        // Her producer belirtilen sayıda item üretir
        for (int i = 0; i < items_per_producer; ++i) {
            int value = dist(rng);  // Rastgele bir değer
            
            // 1. ADIM: Boş bir chunk claim et (lock-free)
            auto ticket = buffer.claim_producer();
            if (!ticket.cpu_ptr) break;  // Shutdown sinyali geldi, çık

            // 2. ADIM: Claim ettiğimiz chunk'a veri yaz
            // ticket.cpu_ptr, chunk'ın başlangıç adresidir
            // chunk_size byte'a kadar yazabiliriz
            int written = std::snprintf(ticket.cpu_ptr, chunk_size, "P%d-%d-%d", id, i, value);
            if (written < 0) written = 0;
            std::size_t bytes_written = static_cast<std::size_t>(written);
            if (bytes_written >= chunk_size) bytes_written = chunk_size - 1;  // null dahil

            // Ek metadata: rfSignal ve size
            *ticket.rf = {id, static_cast<double>(value) / 1000.0};
            *ticket.size_ptr = bytes_written + 1;  // null dahil

            // "GPU" buffer'ına da (short) kopyala / simüle et
            // Byte -> short kopyası: kalan byte'lar üstüne yazılır
            std::size_t gpu_bytes = (chunk_size / sizeof(short)) * sizeof(short);
            if (gpu_bytes == 0) gpu_bytes = sizeof(short);
            std::memset(ticket.gpu_ptr, 0, gpu_bytes);
            std::memcpy(ticket.gpu_ptr, ticket.cpu_ptr,
                        std::min(*ticket.size_ptr, gpu_bytes));

            // 3. ADIM: Chunk'ı doldurduk, consumer'lara açık hale getir
            buffer.commit_producer(ticket);
            
            // İstatistik güncelle
            ++produced_total;
#ifdef LOG_DEBUG
            // Thread-safe log: cout'ların birbirine karışmaması için
            safe_log("P", id, " -> ", ticket.cpu_ptr);
            // Simülasyon: Gerçek uygulamada bu gecikme olmaz
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
#endif
        }
    };

    // ========================================================================
    // Consumer Lambda: Her consumer thread bu fonksiyonu çalıştırır
    // ========================================================================
    auto consumer = [&](int id) {
        while (true) {
            // 1. ADIM: Dolu bir chunk claim et (lock-free)
            auto ticket = buffer.claim_consumer();
            if (!ticket.has_value()) break;  // Shutdown ve buffer boş, çık
            
            // 2. ADIM: Claim ettiğimiz chunk'tan veri oku
            // ticket->cpu_ptr: CPU tarafı (char)
            // ticket->gpu_ptr: GPU tarafı (short, simülasyon)
            // ticket->rf: rfSignal metadata
            // ticket->size_ptr: yazılan byte sayısı
            ++consumed_total;
#ifdef LOG_DEBUG
            // Thread-safe log: cout'ların birbirine karışmaması için
            safe_log("    C", id, " <- ", ticket->cpu_ptr,
                     " | rfSignal=(", ticket->rf->first, ", ",
                     ticket->rf->second, ")"
                     " | size=", *ticket->size_ptr,
                     " | gpu[0]=", ticket->gpu_ptr[0]);
#endif
            
            // 3. ADIM: Chunk'ı okuduk, producer'lara geri ver
            // Chunk yeniden kullanılabilir hale gelir
            buffer.release_consumer(*ticket);
            
#ifdef LOG_DEBUG
            // Simülasyon: Gerçek uygulamada bu gecikme olmaz
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
#endif
        }
    };

    // Thread container'ları
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    // Producer thread'lerini başlat
    // Her producer farklı bir ID ile çalışır (0, 1, 2, ...)
    for (int i = 0; i < producer_count; ++i) {
        producers.emplace_back(producer, i);
    }
    
    // Consumer thread'lerini başlat
    // Her consumer farklı bir ID ile çalışır (0, 1, ...)
    for (int i = 0; i < consumer_count; ++i) {
        consumers.emplace_back(consumer, i);
    }

    // Producer'ların bitmesini bekle
    // Tüm producer'lar item'larını ürettikten sonra devam ederiz
    for (auto& t : producers) t.join();
    
    // Buffer'ı kapat: Consumer'lara "artık yeni data yok" sinyali gönder
    // Bu sayede consumer'lar sonsuz döngüden çıkabilir
    buffer.stop();
    
    // Consumer'ların bitmesini bekle
    // Tüm consumer'lar kalan item'ları işledikten sonra devam ederiz
    for (auto& t : consumers) t.join();

    // Sonuçları yazdır
    // İdeal durumda: produced_total == consumed_total olmalı
#ifdef LOG_DEBUG
    safe_log("Produced: ", produced_total.load(),
             " | Consumed: ", consumed_total.load());
#endif
    return 0;
}


