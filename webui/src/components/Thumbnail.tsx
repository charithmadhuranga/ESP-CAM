interface ThumbnailProps {
  src: string;
  caption: string;
  onClick?: () => void;
}

export function Thumbnail({ src, caption, onClick }: ThumbnailProps) {
  return (
    <div className="thumbnail" onClick={onClick} role="button" tabIndex={0}>
      <div className="thumbnail-box">
        <img src={src} alt={caption} className="thumbnail-img" />
      </div>
      <span className="thumbnail-caption">{caption}</span>
    </div>
  );
}
