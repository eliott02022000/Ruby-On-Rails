Rails.application.routes.draw do
  devise_for :models
  devise_for :users
  resources :users
  resources :pages
  get 'hash/Tags'
  # For details on the DSL available within this file, see http://guides.rubyonrails.org/routing.html
  root 'home#index'

  resources :gazooies, only: [:index, :show, :create]
  resources :news
  resources :comments, only: [:create]
  resources :pages, except: [:index]
  resources :hash_tags, only: [:show]
  resources :profiles, only: [:show, :edit, :update, :follow, :unfollow, :followers, :followees, :mentions] do
    post "follow/:user_id", :to => "profiles#follow"
    delete "unfollow/:user_id", :to => "profiles#unfollow"
    get "followers/", :to => "profiles#followers"
    get "followees/", :to => "profiles#followees"
    get "mentions/", :to => "profiles#mentions"
  end
end


